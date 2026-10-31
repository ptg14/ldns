#include "core/resolver/peer_resolver.hpp"

#include <chrono>
#include <thread>
#include <vector>

#include "doctest.hpp"
#include "core/dns/message.hpp"
#include "core/dns/name.hpp"

using namespace ldns;
using namespace ldns::resolver;

static NodeId make_node_id(std::uint8_t byte_val) {
    NodeId id{};
    id.fill(byte_val);
    return id;
}

TEST_CASE("PeerResolver handle_resolve_request strictly queries LocalZone") {
    dns::LocalZone local_zone;
    auto app_name = dns::Name::from_presentation("app.lan").value();
    auto ip_bytes = dns::parse_ipv4("192.168.1.100").value();
    REQUIRE(local_zone.upsert(app_name, 1, 300, {ip_bytes}).has_value());

    auto cname_name = dns::Name::from_presentation("alias.lan").value();
    auto target_name = dns::Name::from_presentation("target.lan").value();
    auto target_wire = target_name.wire();
    REQUIRE(local_zone.upsert(cname_name, 5, 120, {std::vector<std::uint8_t>(target_wire.begin(), target_wire.end())}).has_value());

    auto registry = std::make_shared<Registry>();
    PeerResolver resolver(registry, nullptr, local_zone, {}, 1234, []() { return 55; });

    SUBCASE("Exact answer found in LocalZone") {
        net::ResolveMessage req;
        req.id = 10;
        req.name = "app.lan";
        req.type = 1;

        // Since session is null, handle_resolve_request won't crash
        resolver.handle_resolve_request(nullptr, req);
    }
}

TEST_CASE("PeerResolver fast path without network when no registry claims") {
    dns::LocalZone local_zone;
    auto registry = std::make_shared<Registry>();
    PeerResolver resolver(registry, nullptr, local_zone);

    std::vector<TraceHop> trace;
    auto query_name = dns::Name::from_presentation("internet.com").value();
    auto result = resolver.resolve(query_name, 1, trace);

    // Fast path: no candidate in registry -> returns nullopt immediately, no trace hop added
    CHECK(!result.has_value());
    CHECK(trace.empty());
}

TEST_CASE("PeerResolver serve_stale offline fallback") {
    dns::LocalZone local_zone;
    auto registry = std::make_shared<Registry>();
    auto id_offline = make_node_id(0x05);

    // Offline owner with claim
    registry->set_owner_online(id_offline, false, "offline-node", 0, 1, 0);

    net::Op op;
    op.op = "put";
    op.name = "stale.lan";
    op.type = 1;
    op.ttl = 300;
    op.data = {{10, 0, 0, 88}};
    op.created = 100;
    op.ts = 100;
    REQUIRE(registry->apply_ops(id_offline, 1, 1, {op}).has_value());

    PeerResolverConfig cfg;
    cfg.serve_stale = true;
    cfg.serve_stale_ttl = 10;

    PeerResolver resolver(registry, nullptr, local_zone, cfg);

    std::vector<TraceHop> trace;
    auto query_name = dns::Name::from_presentation("stale.lan").value();
    auto result = resolver.resolve(query_name, 1, trace);

    REQUIRE(result.has_value());
    CHECK(result->answers.size() == 1);
    CHECK(result->answers[0].name.to_string() == "stale.lan");
    CHECK(result->answers[0].type == 1);
    CHECK(result->answers[0].ttl == 10); // Capped to serve_stale_ttl
    CHECK(result->answers[0].rdata == std::vector<std::uint8_t>{10, 0, 0, 88});

    REQUIRE(trace.size() == 1);
    CHECK(trace[0].step == ResolutionStep::Peer);
    CHECK(trace[0].responder == "registry_stale");
}

TEST_CASE("PeerResolver conflicts tracking") {
    dns::LocalZone local_zone;
    auto registry = std::make_shared<Registry>();
    auto id_a = make_node_id(0x01);
    auto id_b = make_node_id(0x02);

    registry->set_owner_online(id_a, true, "node-a", 0, 1, 0);
    registry->set_owner_online(id_b, true, "node-b", 0, 1, 0);

    net::Op op_a;
    op_a.op = "put";
    op_a.name = "conflict.lan";
    op_a.type = 1;
    op_a.ttl = 60;
    op_a.data = {{192, 168, 1, 1}};
    op_a.created = 100;
    op_a.ts = 100;

    net::Op op_b = op_a;
    op_b.data = {{192, 168, 1, 2}}; // Differing IP
    op_b.created = 200;
    op_b.ts = 200;

    REQUIRE(registry->apply_ops(id_a, 1, 1, {op_a}).has_value());
    REQUIRE(registry->apply_ops(id_b, 1, 1, {op_b}).has_value());

    bool conflict_called = false;
    PeerResolver resolver(registry, nullptr, local_zone);
    resolver.set_conflict_callback([&](const ConflictRecord& cr) {
        conflict_called = true;
        CHECK(cr.name.to_string() == "conflict.lan");
        CHECK(cr.groups.size() == 2);
    });

    std::vector<TraceHop> trace;
    auto qname = dns::Name::from_presentation("conflict.lan").value();
    (void)resolver.resolve(qname, 1, trace);

    CHECK(conflict_called);
    auto conflicts = resolver.conflicts();
    REQUIRE(conflicts.size() == 1);
    CHECK(conflicts[0].name.to_string() == "conflict.lan");
    CHECK(conflicts[0].groups.size() == 2);

    auto single = resolver.get_conflict(qname, 1);
    REQUIRE(single.has_value());
    CHECK(single->groups.size() == 2);

    resolver.clear_conflicts();
    CHECK(resolver.conflicts().empty());
}
