#include "doctest.hpp"

#include <asio.hpp>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "core/net/peer_manager.hpp"
#include "core/net/protocol_message.hpp"
#include "core/registry/registry.hpp"
#include "core/registry/sync_manager.hpp"
#include "core/store/store.hpp"
#include "core/util/id.hpp"

using namespace ldns;
using namespace ldns::net;

namespace {

StoreOptions mem_store() {
    StoreOptions opt;
    opt.path = ":memory:";
    return opt;
}

}  // namespace

TEST_CASE("SyncManager UPDATE gap detection, buffering, and recovery") {
    auto store_res = Store::open(mem_store());
    REQUIRE(store_res.has_value());
    auto& store = store_res.value();
    (void)store.identity();

    Registry registry;
    NodeId self_id{};
    self_id[0] = 0x01;
    NodeId peer_id{};
    peer_id[0] = 0x02;

    SyncManagerOptions opts;
    opts.self_id = self_id;
    opts.self_name = "node-one";
    opts.self_epoch = 1;

    std::vector<std::string> invalidated;
    auto sync_mgr = SyncManager::create(opts, store, registry, nullptr, [&](const std::string& name) {
        invalidated.push_back(name);
    });

    registry.set_owner_online(peer_id, true, "peer-node", 100);

    // Initial state: peer has seq = 1
    std::vector<Op> initial_ops = {
        {"put", "base.lan", 1, 60, {{10, 0, 0, 1}}, 1, 1},
    };
    REQUIRE(registry.apply_ops(peer_id, 1, 1, initial_ops).has_value());
    CHECK(registry.snapshot()->find_owner(peer_id)->last_seq == 1);

    // Peer sends update with seq = 3 (gap! expecting seq = 2)
    UpdateMessage update3;
    update3.owner = peer_id;
    update3.epoch = 1;
    update3.seq = 3;
    update3.ops = {{"put", "seq3.lan", 1, 60, {{10, 0, 0, 3}}, 3, 3}};

    ParsedMessage msg3;
    msg3.type = MessageType::Update;
    msg3.update = update3;

    // Simulate receiving update 3: it should be buffered, not applied yet!
    sync_mgr->on_session_message(nullptr, msg3);
    CHECK(registry.snapshot()->find_owner(peer_id)->last_seq == 1);
    auto app3_name = dns::Name::from_presentation("seq3.lan").value();
    CHECK(registry.snapshot()->find_claims(app3_name, 1).empty());

    // Peer now sends missing seq = 2
    UpdateMessage update2;
    update2.owner = peer_id;
    update2.epoch = 1;
    update2.seq = 2;
    update2.ops = {{"put", "seq2.lan", 1, 60, {{10, 0, 0, 2}}, 2, 2}};

    ParsedMessage msg2;
    msg2.type = MessageType::Update;
    msg2.update = update2;

    // Simulate receiving update 2:
    // Seq 2 is applied, then buffered seq 3 is automatically applied!
    sync_mgr->on_session_message(nullptr, msg2);

    auto snap = registry.snapshot();
    auto owner = snap->find_owner(peer_id);
    REQUIRE(owner != nullptr);
    CHECK(owner->last_seq == 3);

    auto app2_name = dns::Name::from_presentation("seq2.lan").value();
    CHECK(snap->find_claims(app2_name, 1).size() == 1);
    CHECK(snap->find_claims(app3_name, 1).size() == 1);

    // Check store peer_records
    auto stored_peers = store.peer_records(peer_id);
    REQUIRE(stored_peers.has_value());
    CHECK(stored_peers.value().size() == 2);
}

TEST_CASE("SyncManager broadcast_local_update records ops in store and registry") {
    auto store_res = Store::open(mem_store());
    REQUIRE(store_res.has_value());
    auto& store = store_res.value();
    auto ident = store.identity().value();

    Registry registry;
    SyncManagerOptions opts;
    opts.self_id = ident.node_id;
    opts.self_name = "node-one";
    opts.self_epoch = ident.epoch;

    std::vector<std::string> invalidated;
    auto sync_mgr = SyncManager::create(opts, store, registry, nullptr, [&](const std::string& name) {
        invalidated.push_back(name);
    });

    std::vector<Op> ops = {
        {"put", "myhost.lan", 1, 300, {{192, 168, 1, 10}}, 10, 10},
    };

    REQUIRE(sync_mgr->broadcast_local_update(ops).has_value());

    auto snap = registry.snapshot();
    auto my_owner = snap->find_owner(ident.node_id);
    REQUIRE(my_owner != nullptr);
    CHECK(my_owner->records.size() == 1);

    auto myhost_name = dns::Name::from_presentation("myhost.lan").value();
    auto claims = snap->find_claims(myhost_name, 1);
    REQUIRE(claims.size() == 1);
    CHECK(claims[0].owner_id == ident.node_id);

    CHECK_FALSE(invalidated.empty());
    CHECK(invalidated[0] == "myhost.lan");
}

TEST_CASE("SyncManager multi-node convergence over loopback PeerManagers") {
    asio::io_context io;

    // Node 1 (smaller node_id)
    NodeId id1{};
    id1.fill(0x01);
    auto store1 = Store::open(mem_store()).value();
    (void)store1.identity();
    Registry registry1;

    // Node 2 (larger node_id)
    NodeId id2{};
    id2.fill(0x02);
    auto store2 = Store::open(mem_store()).value();
    (void)store2.identity();
    Registry registry2;

    PeerManagerConfig cfg1;
    cfg1.node_id = id1;
    cfg1.node_name = "node-one";
    cfg1.network_name = "test-sync-net";
    cfg1.sync_port = 0;

    PeerManagerConfig cfg2;
    cfg2.node_id = id2;
    cfg2.node_name = "node-two";
    cfg2.network_name = "test-sync-net";
    cfg2.sync_port = 0;

    std::atomic<bool> node1_peer_up{false};
    std::atomic<bool> node2_peer_up{false};

    SyncManagerOptions opts1;
    opts1.self_id = id1;
    opts1.self_name = "node-one";
    opts1.self_epoch = 1;

    SyncManagerOptions opts2;
    opts2.self_id = id2;
    opts2.self_name = "node-two";
    opts2.self_epoch = 1;

    std::shared_ptr<SyncManager> sync1;
    std::shared_ptr<SyncManager> sync2;

    auto pm1 = PeerManager::create(io, cfg1, [&](const PeerRecord& peer, bool online) {
        if (online) node1_peer_up = true;
        if (sync1) sync1->on_peer_event(peer, online);
    });

    auto pm2 = PeerManager::create(io, cfg2, [&](const PeerRecord& peer, bool online) {
        if (online) node2_peer_up = true;
        if (sync2) sync2->on_peer_event(peer, online);
    });

    sync1 = SyncManager::create(opts1, store1, registry1, pm1);
    sync2 = SyncManager::create(opts2, store2, registry2, pm2);

    sync1->start();
    sync2->start();

    REQUIRE(pm1->start().has_value());
    REQUIRE(pm2->start().has_value());

    const auto port2 = pm2->bound_port();
    REQUIRE(port2 > 0);

    // Node 1 local record
    std::vector<Op> n1_ops = {
        {"put", "node1.lan", 1, 120, {{10, 1, 1, 1}}, 1, 1},
    };
    {
        auto tx = store1.begin();
        (void)store1.log_op({1, "put", "node1.lan", 1, 120, {{10, 1, 1, 1}}, 1, 1});
        (void)store1.bump_seq();
        (void)tx.value().commit();
    }
    REQUIRE(sync1->broadcast_local_update(n1_ops).has_value());

    // Node 1 dials Node 2
    DiscoveredPeer d2;
    d2.node_id = id2;
    d2.name = "node-two";
    d2.sync_port = port2;
    d2.dns_port = 53;
    d2.source_endpoint = asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), port2);
    d2.ips = {"127.0.0.1"};

    pm1->on_beacon_discovered(d2);

    for (int i = 0; i < 100 && (!node1_peer_up || !node2_peer_up); ++i) {
        io.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    CHECK(node1_peer_up);
    CHECK(node2_peer_up);

    // Wait for sync handshake (SYNC_REQ / SYNC_DATA) to propagate
    for (int i = 0; i < 50; ++i) {
        io.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Node 2 should have learned node1.lan via sync!
    auto snap2 = registry2.snapshot();
    auto n1_name = dns::Name::from_presentation("node1.lan").value();
    auto claims_on_node2 = snap2->find_claims(n1_name, 1);
    REQUIRE(claims_on_node2.size() == 1);
    CHECK(claims_on_node2[0].owner_id == id1);

    // Now Node 2 adds a domain and broadcasts UPDATE!
    std::vector<Op> n2_ops = {
        {"put", "node2.lan", 1, 60, {{10, 2, 2, 2}}, 2, 2},
    };
    {
        auto tx = store2.begin();
        (void)store2.log_op({1, "put", "node2.lan", 1, 60, {{10, 2, 2, 2}}, 2, 2});
        (void)store2.bump_seq();
        (void)tx.value().commit();
    }
    REQUIRE(sync2->broadcast_local_update(n2_ops).has_value());

    // Deliver update over TCP
    for (int i = 0; i < 50; ++i) {
        io.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Node 1 should now have node2.lan!
    auto snap1 = registry1.snapshot();
    auto n2_name = dns::Name::from_presentation("node2.lan").value();
    auto claims_on_node1 = snap1->find_claims(n2_name, 1);
    REQUIRE(claims_on_node1.size() == 1);
    CHECK(claims_on_node1[0].owner_id == id2);

    // Both nodes converged!
    snap2 = registry2.snapshot();
    CHECK(snap1->total_record_count() == 2);
    CHECK(snap2->total_record_count() == 2);

    sync1->stop();
    sync2->stop();
    pm1->stop();
    pm2->stop();
}

TEST_CASE("SyncManager 3-node gossip relay: A -> B -> C") {
    asio::io_context io;

    // Node A (0x01)
    NodeId id_a{};
    id_a.fill(0x01);
    auto store_a = Store::open(mem_store()).value();
    auto ident_a = store_a.identity().value();
    Registry reg_a;

    // Node B (0x02)
    NodeId id_b{};
    id_b.fill(0x02);
    auto store_b = Store::open(mem_store()).value();
    auto ident_b = store_b.identity().value();
    Registry reg_b;

    // Node C (0x03)
    NodeId id_c{};
    id_c.fill(0x03);
    auto store_c = Store::open(mem_store()).value();
    auto ident_c = store_c.identity().value();
    Registry reg_c;

    PeerManagerConfig cfg_a;
    cfg_a.node_id = id_a;
    cfg_a.node_name = "node-a";
    cfg_a.network_name = "test-gossip";
    cfg_a.sync_port = 0;
    cfg_a.my_epoch = ident_a.epoch;
    cfg_a.my_seq = ident_a.seq;

    PeerManagerConfig cfg_b;
    cfg_b.node_id = id_b;
    cfg_b.node_name = "node-b";
    cfg_b.network_name = "test-gossip";
    cfg_b.sync_port = 0;
    cfg_b.my_epoch = ident_b.epoch;
    cfg_b.my_seq = ident_b.seq;

    PeerManagerConfig cfg_c;
    cfg_c.node_id = id_c;
    cfg_c.node_name = "node-c";
    cfg_c.network_name = "test-gossip";
    cfg_c.sync_port = 0;
    cfg_c.my_epoch = ident_c.epoch;
    cfg_c.my_seq = ident_c.seq;

    std::shared_ptr<SyncManager> sync_a;
    std::shared_ptr<SyncManager> sync_b;
    std::shared_ptr<SyncManager> sync_c;

    std::atomic<bool> ab_connected{false};
    std::atomic<bool> bc_connected{false};

    auto pm_a = PeerManager::create(io, cfg_a, [&](const PeerRecord& peer, bool online) {
        if (online && peer.node_id == id_b) ab_connected = true;
        if (sync_a) sync_a->on_peer_event(peer, online);
    });

    auto pm_b = PeerManager::create(io, cfg_b, [&](const PeerRecord& peer, bool online) {
        if (online) {
            if (peer.node_id == id_a) ab_connected = true;
            if (peer.node_id == id_c) bc_connected = true;
        }
        if (sync_b) sync_b->on_peer_event(peer, online);
    });

    auto pm_c = PeerManager::create(io, cfg_c, [&](const PeerRecord& peer, bool online) {
        if (online && peer.node_id == id_b) bc_connected = true;
        if (sync_c) sync_c->on_peer_event(peer, online);
    });

    SyncManagerOptions opts_a;
    opts_a.self_id = id_a;
    opts_a.self_name = "node-a";
    opts_a.self_epoch = ident_a.epoch;

    SyncManagerOptions opts_b;
    opts_b.self_id = id_b;
    opts_b.self_name = "node-b";
    opts_b.self_epoch = ident_b.epoch;

    SyncManagerOptions opts_c;
    opts_c.self_id = id_c;
    opts_c.self_name = "node-c";
    opts_c.self_epoch = ident_c.epoch;

    sync_a = SyncManager::create(opts_a, store_a, reg_a, pm_a);
    sync_b = SyncManager::create(opts_b, store_b, reg_b, pm_b);
    sync_c = SyncManager::create(opts_c, store_c, reg_c, pm_c);

    sync_a->start();
    sync_b->start();
    sync_c->start();

    REQUIRE(pm_a->start().has_value());
    REQUIRE(pm_b->start().has_value());
    REQUIRE(pm_c->start().has_value());

    const auto port_b = pm_b->bound_port();
    REQUIRE(port_b > 0);

    // Node A dials Node B
    DiscoveredPeer d_b;
    d_b.node_id = id_b;
    d_b.name = "node-b";
    d_b.sync_port = port_b;
    d_b.source_endpoint = asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), port_b);
    d_b.ips = {"127.0.0.1"};
    pm_a->on_beacon_discovered(d_b);

    // Node B dials Node C: id_b < id_c, so B dials C!
    const auto port_c = pm_c->bound_port();
    REQUIRE(port_c > 0);
    DiscoveredPeer d_c;
    d_c.node_id = id_c;
    d_c.name = "node-c";
    d_c.sync_port = port_c;
    d_c.source_endpoint = asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), port_c);
    d_c.ips = {"127.0.0.1"};
    pm_b->on_beacon_discovered(d_c);

    for (int i = 0; i < 100 && (!ab_connected || !bc_connected); ++i) {
        io.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    CHECK(ab_connected);
    CHECK(bc_connected);

    // Now Node A registers a record and broadcasts UPDATE
    std::vector<Op> a_ops = {
        {"put", "origin-a.lan", 1, 300, {{192, 168, 1, 100}}, 1, 1},
    };
    {
        auto tx = store_a.begin();
        (void)store_a.log_op({1, "put", "origin-a.lan", 1, 300, {{192, 168, 1, 100}}, 1, 1});
        (void)store_a.bump_seq();
        (void)tx.value().commit();
    }
    REQUIRE(sync_a->broadcast_local_update(a_ops).has_value());

    auto origin_name = dns::Name::from_presentation("origin-a.lan").value();

    // Run event loop: A sends to B; B receives, applies, and relays to C!
    for (int i = 0; i < 200; ++i) {
        io.poll();
        if (!reg_c.snapshot()->find_claims(origin_name, 1).empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Verify Node B received the update
    auto snap_b = reg_b.snapshot();
    auto claims_b = snap_b->find_claims(origin_name, 1);
    REQUIRE(claims_b.size() == 1);
    CHECK(claims_b[0].owner_id == id_a);

    // Verify Node C received the update via gossip relay from Node B!
    auto snap_c = reg_c.snapshot();

    auto claims_c = snap_c->find_claims(origin_name, 1);
    REQUIRE(claims_c.size() == 1);
    CHECK(claims_c[0].owner_id == id_a);

    sync_a->stop();
    sync_b->stop();
    sync_c->stop();
    pm_a->stop();
    pm_b->stop();
    pm_c->stop();
}

TEST_CASE("SyncManager offline purge removes expired peer records from store and registry") {
    auto store_res = Store::open(mem_store());
    REQUIRE(store_res.has_value());
    auto& store = store_res.value();
    (void)store.identity();

    Registry registry;
    NodeId self_id{};
    self_id[0] = 0x01;
    NodeId peer_id{};
    peer_id[0] = 0x99;

    SyncManagerOptions opts;
    opts.self_id = self_id;
    opts.self_name = "node-one";
    opts.self_epoch = 1;
    opts.offline_retention = std::chrono::seconds(60); // 60 seconds retention

    auto sync_mgr = SyncManager::create(opts, store, registry, nullptr);

    // Put peer record in store
    PeerRecordRow row;
    row.owner_id = peer_id;
    row.name = "old-peer.lan";
    row.type = 1;
    row.ttl = 120;
    row.data = {{1, 2, 3, 4}};
    row.created_at = 10;
    REQUIRE(store.put_peer_record(row).has_value());
    CHECK(store.peer_records(peer_id).value().size() == 1);

    // Add peer to registry, marked offline with last_seen = 100
    registry.set_owner_online(peer_id, false, "old-peer", 100);
    std::vector<Op> ops = {{"put", "old-peer.lan", 1, 120, {{1, 2, 3, 4}}, 10, 10}};
    REQUIRE(registry.apply_ops(peer_id, 1, 1, ops).has_value());
    CHECK(registry.snapshot()->find_owner(peer_id) != nullptr);

    // Purge at now_ts = 150 (elapsed 50s < 60s) -> not purged
    sync_mgr->purge_offline(150);
    CHECK(registry.snapshot()->find_owner(peer_id) != nullptr);
    CHECK(store.peer_records(peer_id).value().size() == 1);

    // Purge at now_ts = 170 (elapsed 70s >= 60s) -> purged!
    sync_mgr->purge_offline(170);
    CHECK(registry.snapshot()->find_owner(peer_id) == nullptr);
    CHECK(store.peer_records(peer_id).value().empty());
}
