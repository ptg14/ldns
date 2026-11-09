#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "doctest.hpp"
#include "core/dns/local_zone.hpp"
#include "core/dns/message.hpp"
#include "core/dns/name.hpp"
#include "core/net/peer_manager.hpp"
#include "core/registry/registry.hpp"
#include "core/registry/sync_manager.hpp"
#include "core/resolver/cache.hpp"
#include "core/resolver/peer_resolver.hpp"
#include "core/resolver/pipeline.hpp"
#include "core/store/store.hpp"
#include "core/util/id.hpp"

using namespace ldns;

namespace {

struct TestNode {
    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> work;
    std::unique_ptr<Store> store;
    std::shared_ptr<Registry> registry;
    dns::LocalZone zone;
    std::shared_ptr<net::PeerManager> pm;
    std::shared_ptr<SyncManager> sync;
    std::shared_ptr<resolver::PeerResolver> peer_resolver;
    std::shared_ptr<resolver::DnsCache> cache;
    std::shared_ptr<resolver::ResolverPipeline> pipeline;
    NodeId id{};
    std::uint16_t sync_port{0};
    std::thread io_thread;

    TestNode(std::uint8_t id_byte, std::string name, std::chrono::milliseconds hedge_ms = std::chrono::milliseconds(100))
        : work(asio::make_work_guard(io)) {
        id.fill(id_byte);

        StoreOptions s_opt;
        s_opt.path = ":memory:";
        store = std::make_unique<Store>(Store::open(s_opt).value());
        auto ident = store->identity().value();

        registry = std::make_shared<Registry>();

        net::PeerManagerConfig pm_cfg;
        pm_cfg.node_id = id;
        pm_cfg.node_name = name;
        pm_cfg.network_name = "test-net";
        pm_cfg.sync_port = 0;
        pm_cfg.my_epoch = ident.epoch;
        pm_cfg.listen_addresses = {"127.0.0.1"};
        pm = net::PeerManager::create(io, pm_cfg, [this](const net::PeerRecord& peer, bool online) {
            if (sync) {
                sync->on_peer_event(peer, online);
            }
        });
        REQUIRE(pm->start().has_value());
        sync_port = pm->bound_port();
        REQUIRE(sync_port > 0);

        SyncManagerOptions sm_opts;
        sm_opts.self_id = id;
        sm_opts.self_name = name;
        sm_opts.self_epoch = ident.epoch;
        sync = SyncManager::create(sm_opts, *store, *registry, pm);
        sync->start();

        resolver::PeerResolverConfig pr_cfg;
        pr_cfg.hedge_ms = hedge_ms;
        pr_cfg.budget_ms = std::chrono::milliseconds(2000);
        pr_cfg.peer_ttl_cap = 30;
        peer_resolver = std::make_shared<resolver::PeerResolver>(
            registry, pm, zone, pr_cfg, ident.epoch, [this]() { return store->identity().value().seq; });
        peer_resolver->set_sync_trigger([this](const NodeId& owner) {
            sync->request_sync_for_owner(owner);
        });

        pm->set_resolve_handlers(
            [this](auto s, const auto& req) { peer_resolver->handle_resolve_request(s, req); },
            [this](auto s, const auto& resp) { peer_resolver->handle_resolve_response(s, resp); });

        cache = std::make_shared<resolver::DnsCache>();

        resolver::PipelineConfig pl_cfg;
        pl_cfg.peer_ttl_cap = 30;
        pipeline = std::make_shared<resolver::ResolverPipeline>(zone, cache, nullptr, nullptr, pl_cfg);
        pipeline->set_peer_hook([this](const dns::Name& n, std::uint16_t t, std::vector<resolver::TraceHop>& trace) {
            return peer_resolver->resolve(n, t, trace);
        });

        io_thread = std::thread([this]() { io.run(); });
    }

    ~TestNode() {
        work.reset();
        io.stop();
        if (io_thread.joinable()) io_thread.join();
        if (sync) sync->stop();
        if (pm) pm->stop();
    }

    void add_a(const std::string& name_str, const std::string& ip_str, std::uint32_t ttl = 300, std::uint64_t created = 100) {
        auto name = dns::Name::from_presentation(name_str).value();
        auto ip = dns::parse_ipv4(ip_str).value();
        REQUIRE(zone.upsert(name, 1, ttl, {ip}).has_value());

        auto tx = store->begin();
        auto seq = store->bump_seq().value();
        net::Op op{"put", name_str, 1, ttl, {ip}, created, created};
        REQUIRE(store->log_op({seq, op.op, op.name, op.type, op.ttl, op.data, op.created, op.ts}).has_value());
        REQUIRE(tx.value().commit().has_value());
        REQUIRE(sync->broadcast_local_update({op}).has_value());
    }

    void add_cname(const std::string& name_str, const std::string& target_str, std::uint32_t ttl = 300, std::uint64_t created = 100) {
        auto name = dns::Name::from_presentation(name_str).value();
        auto target = dns::Name::from_presentation(target_str).value();
        auto target_wire = target.wire();
        std::vector<std::uint8_t> target_bytes(target_wire.begin(), target_wire.end());
        REQUIRE(zone.upsert(name, 5, ttl, {target_bytes}).has_value());

        auto tx = store->begin();
        auto seq = store->bump_seq().value();
        net::Op op{"put", name_str, 5, ttl, {target_bytes}, created, created};
        REQUIRE(store->log_op({seq, op.op, op.name, op.type, op.ttl, op.data, op.created, op.ts}).has_value());
        REQUIRE(tx.value().commit().has_value());
        REQUIRE(sync->broadcast_local_update({op}).has_value());
    }

    void connect_to(TestNode& other) {
        net::DiscoveredPeer dp;
        dp.node_id = other.id;
        dp.name = "other";
        dp.sync_port = other.sync_port;
        dp.dns_port = 53;
        dp.ips = {"127.0.0.1"};
        dp.source_endpoint = asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), other.sync_port);
        pm->on_beacon_discovered(dp);
    }

    resolver::PipelineResult resolve(const std::string& name_str, std::uint16_t type = 1, bool trace = false) {
        auto name = dns::Name::from_presentation(name_str).value();
        dns::Message query;
        query.id = 0x1234;
        query.flags = dns::kRd;
        query.questions.push_back(dns::Question{name, type, dns::kClassIn});
        return pipeline->resolve(query, {}, trace);
    }
};

void link_nodes(TestNode& n1, TestNode& n2) {
    n1.connect_to(n2);
    n2.connect_to(n1);
}

void wait_for_connection(TestNode& n1, TestNode& n2, int max_ms = 4000) {
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start).count() < max_ms) {
        auto p1 = n1.pm->get_peer(n2.id);
        auto p2 = n2.pm->get_peer(n1.id);
        if (p1 && p1->is_online && p2 && p2->is_online) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    auto p1 = n1.pm->get_peer(n2.id);
    auto p2 = n2.pm->get_peer(n1.id);
    REQUIRE(p1.has_value());
    REQUIRE(p1->is_online);
    REQUIRE(p2.has_value());
    REQUIRE(p2->is_online);
}

void wait_for_claim(TestNode& node, const std::string& name_str, std::uint16_t type, int max_ms = 4000) {
    auto name = dns::Name::from_presentation(name_str).value();
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start).count() < max_ms) {
        auto snap = node.registry->snapshot();
        auto claims = snap->find_claims(name, type);
        if (!claims.empty()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    auto snap = node.registry->snapshot();
    REQUIRE_FALSE(snap->find_claims(name, type).empty());
}

}  // namespace

TEST_CASE("E2E Scenario 1 (AC-1.2): 2-node peer resolution, TTL cap 30s, and caching") {
    TestNode nodeA(0x01, "node-a");
    TestNode nodeB(0x02, "node-b");

    nodeA.add_a("app.lan", "192.168.1.10", 300);

    link_nodes(nodeA, nodeB);
    wait_for_connection(nodeA, nodeB);
    wait_for_claim(nodeB, "app.lan", 1);

    // 1. First resolution: queries peer over wire
    auto res1 = nodeB.resolve("app.lan", 1);
    CHECK(res1.resolved_step == resolver::ResolutionStep::Peer);
    CHECK(res1.from_cache == false);
    REQUIRE(res1.response.answers.size() == 1);
    CHECK(res1.response.answers[0].name.to_string() == "app.lan");
    CHECK(res1.response.answers[0].type == 1);
    CHECK(res1.response.answers[0].rdata == dns::parse_ipv4("192.168.1.10").value());
    // TTL capped to 30s (peer_ttl_cap) per SPEC §6.3.5
    CHECK(res1.response.answers[0].ttl <= 30);

    // 2. Second resolution: served directly from cache
    auto res2 = nodeB.resolve("app.lan", 1);
    CHECK(res2.resolved_step == resolver::ResolutionStep::Cache);
    CHECK(res2.from_cache == true);
    REQUIRE(res2.response.answers.size() == 1);
    CHECK(res2.response.answers[0].rdata == dns::parse_ipv4("192.168.1.10").value());
}

TEST_CASE("E2E Scenario 2 (AC-1.3): 3-node tie-breaking and conflict tracking") {
    TestNode nodeA(0x01, "node-a");
    TestNode nodeB(0x02, "node-b");
    TestNode nodeC(0x03, "node-c");

    // Node A: created at 100 -> 10.0.0.1
    // Node B: created at 200 -> 10.0.0.2
    nodeA.add_a("conflict.lan", "10.0.0.1", 120, 100);
    nodeB.add_a("conflict.lan", "10.0.0.2", 120, 200);

    link_nodes(nodeA, nodeC);
    link_nodes(nodeB, nodeC);
    wait_for_connection(nodeA, nodeC);
    wait_for_connection(nodeB, nodeC);

    // Wait until Node C learns both claims
    auto qname = dns::Name::from_presentation("conflict.lan").value();
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start).count() < 4000) {
        auto snap = nodeC.registry->snapshot();
        auto claims = snap->find_claims(qname, 1);
        if (claims.size() >= 2) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    CHECK(nodeC.registry->snapshot()->find_claims(qname, 1).size() >= 2);

    // Node C resolves conflict.lan
    auto res = nodeC.resolve("conflict.lan", 1);
    CHECK(res.resolved_step == resolver::ResolutionStep::Peer);
    REQUIRE(res.response.answers.size() == 1);

    // Deterministic tie-breaker: Group A (created at 100) wins over Group B (created at 200)
    CHECK(res.response.answers[0].rdata == dns::parse_ipv4("10.0.0.1").value());

    // Conflict was tracked in Node C's PeerResolver
    auto confs = nodeC.peer_resolver->conflicts();
    REQUIRE(confs.size() == 1);
    CHECK(confs[0].name.to_string() == "conflict.lan");
    CHECK(confs[0].groups.size() == 2);
}

TEST_CASE("E2E Scenario 3 (AC-1.4): Multi-hop CNAME across peers followed end-to-end") {
    TestNode nodeA(0x01, "node-a");
    TestNode nodeB(0x02, "node-b");
    TestNode nodeC(0x03, "node-c");

    // Node A holds CNAME: alias.lan -> target.lan
    // Node B holds A: target.lan -> 1.2.3.4
    nodeA.add_cname("alias.lan", "target.lan", 300);
    nodeB.add_a("target.lan", "1.2.3.4", 300);

    link_nodes(nodeA, nodeC);
    link_nodes(nodeB, nodeC);
    wait_for_connection(nodeA, nodeC);
    wait_for_connection(nodeB, nodeC);

    wait_for_claim(nodeC, "alias.lan", 5);
    wait_for_claim(nodeC, "target.lan", 1);

    // Node C resolves alias.lan
    auto res = nodeC.resolve("alias.lan", 1);
    CHECK(res.resolved_step == resolver::ResolutionStep::Peer);

    // Answers include both the CNAME and final A record
    REQUIRE(res.response.answers.size() == 2);
    CHECK(res.response.answers[0].name.to_string() == "alias.lan");
    CHECK(res.response.answers[0].type == 5);
    CHECK(res.response.answers[1].name.to_string() == "target.lan");
    CHECK(res.response.answers[1].type == 1);
    CHECK(res.response.answers[1].rdata == dns::parse_ipv4("1.2.3.4").value());
}

TEST_CASE("E2E Scenario 4: Hedging query when first peer delays") {
    // Node C uses 60ms hedge timer
    TestNode nodeA(0x01, "node-a");
    TestNode nodeB(0x02, "node-b");
    TestNode nodeC(0x03, "node-c", std::chrono::milliseconds(60));

    // Both Node A and Node B host identical RRset for hedged.lan -> 10.9.9.9
    // They belong to the same group with 2 votes!
    nodeA.add_a("hedged.lan", "10.9.9.9", 120, 100);
    nodeB.add_a("hedged.lan", "10.9.9.9", 120, 100);

    // Intentionally inject delay on Node A's response handler
    nodeA.pm->set_resolve_handlers(
        [&nodeA](auto s, const auto& req) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            nodeA.peer_resolver->handle_resolve_request(s, req);
        },
        [&nodeA](auto s, const auto& resp) {
            nodeA.peer_resolver->handle_resolve_response(s, resp);
        });

    link_nodes(nodeA, nodeC);
    link_nodes(nodeB, nodeC);
    wait_for_connection(nodeA, nodeC);
    wait_for_connection(nodeB, nodeC);

    auto qname = dns::Name::from_presentation("hedged.lan").value();
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start).count() < 4000) {
        auto snap = nodeC.registry->snapshot();
        auto claims = snap->find_claims(qname, 1);
        if (claims.size() >= 2) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    auto t_start = std::chrono::steady_clock::now();
    auto res = nodeC.resolve("hedged.lan", 1);
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t_start).count();

    CHECK(res.resolved_step == resolver::ResolutionStep::Peer);
    REQUIRE(res.response.answers.size() == 1);
    CHECK(res.response.answers[0].rdata == dns::parse_ipv4("10.9.9.9").value());

    // Hedging kicked in around ~60ms and Node B answered promptly, well before Node A's 500ms delay!
    CHECK(duration_ms < 400);
}
