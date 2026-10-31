#include "doctest.hpp"

#include "core/registry/registry.hpp"

using namespace ldns;

TEST_CASE("Registry initial state and empty snapshot") {
    Registry registry;
    auto snap = registry.snapshot();
    REQUIRE(snap != nullptr);
    CHECK(snap->owners().empty());
    CHECK(snap->total_record_count() == 0);

    auto name = dns::Name::from_presentation("app.lan").value();
    auto claims = snap->find_claims(name, 1);
    CHECK(claims.empty());
}

TEST_CASE("Registry apply_ops put and del with claims query and wildcards") {
    Registry registry;

    NodeId node_a{};
    node_a[0] = 0xAA;

    std::vector<RecordEvent> events;
    registry.add_listener([&](const RecordEvent& ev) {
        events.push_back(ev);
    });

    registry.set_owner_online(node_a, true, "node-a", 1000);

    // Apply put ops at seq 1
    std::vector<Op> ops1 = {
        {"put", "app.lan", 1, 300, {{192, 168, 1, 50}}, 10, 10},
        {"put", "*.corp.lan", 1, 60, {{10, 0, 0, 1}}, 11, 11},
    };

    auto res = registry.apply_ops(node_a, 1, 1, ops1);
    REQUIRE(res.has_value());
    CHECK(events.size() == 2);
    CHECK(events[0].kind == RecordEventKind::Put);
    CHECK(events[0].record.name_str == "app.lan");
    CHECK(events[1].record.name_str == "*.corp.lan");

    auto snap = registry.snapshot();
    REQUIRE(snap != nullptr);
    CHECK(snap->total_record_count() == 2);

    auto owner_a = snap->find_owner(node_a);
    REQUIRE(owner_a != nullptr);
    CHECK(owner_a->epoch == 1);
    CHECK(owner_a->last_seq == 1);
    CHECK(owner_a->online == true);

    // Exact match
    auto app_name = dns::Name::from_presentation("app.lan").value();
    auto app_claims = snap->find_claims(app_name, 1);
    REQUIRE(app_claims.size() == 1);
    CHECK(app_claims[0].owner_id == node_a);
    CHECK(app_claims[0].owner_online == true);
    CHECK(app_claims[0].record.ttl == 300);

    // Wildcard match for sub.corp.lan -> matches *.corp.lan
    auto sub_name = dns::Name::from_presentation("sub.corp.lan").value();
    auto sub_claims = snap->find_claims(sub_name, 1);
    REQUIRE(sub_claims.size() == 1);
    CHECK(sub_claims[0].record.name_str == "*.corp.lan");

    // Deep wildcard match for deep.sub.corp.lan -> matches *.corp.lan
    auto deep_name = dns::Name::from_presentation("deep.sub.corp.lan").value();
    auto deep_claims = snap->find_claims(deep_name, 1);
    REQUIRE(deep_claims.size() == 1);
    CHECK(deep_claims[0].record.name_str == "*.corp.lan");

    // Non-existent domain
    auto other_name = dns::Name::from_presentation("other.lan").value();
    CHECK(snap->find_claims(other_name, 1).empty());

    // Duplicate seq 1 is ignored
    events.clear();
    REQUIRE(registry.apply_ops(node_a, 1, 1, ops1).has_value());
    CHECK(events.empty());

    // Apply del op at seq 2
    std::vector<Op> ops2 = {
        {"del", "app.lan", 1, 0, {}, 10, 15},
    };
    REQUIRE(registry.apply_ops(node_a, 1, 2, ops2).has_value());
    CHECK(events.size() == 1);
    CHECK(events[0].kind == RecordEventKind::Del);

    snap = registry.snapshot();
    CHECK(snap->total_record_count() == 1);
    CHECK(snap->find_claims(app_name, 1).empty());
}

TEST_CASE("Registry epoch transition and stale epoch rejection") {
    Registry registry;
    NodeId node_a{};
    node_a[0] = 0xAA;

    std::vector<Op> ops1 = {
        {"put", "app.lan", 1, 300, {{192, 168, 1, 50}}, 10, 10},
    };
    REQUIRE(registry.apply_ops(node_a, 1, 1, ops1).has_value());
    CHECK(registry.snapshot()->total_record_count() == 1);

    // Stale epoch (0 < 1) rejected
    auto err = registry.apply_ops(node_a, 0, 2, ops1);
    CHECK_FALSE(err.has_value());

    // New epoch (2 > 1) resets old records
    std::vector<Op> ops_new_epoch = {
        {"put", "new.lan", 1, 120, {{10, 0, 0, 99}}, 20, 20},
    };
    REQUIRE(registry.apply_ops(node_a, 2, 1, ops_new_epoch).has_value());
    auto snap = registry.snapshot();
    CHECK(snap->total_record_count() == 1);

    auto app_name = dns::Name::from_presentation("app.lan").value();
    CHECK(snap->find_claims(app_name, 1).empty());

    auto new_name = dns::Name::from_presentation("new.lan").value();
    CHECK(snap->find_claims(new_name, 1).size() == 1);
}

TEST_CASE("Registry reset_owner_snapshot") {
    Registry registry;
    NodeId node_a{};
    node_a[0] = 0xAA;

    std::vector<Op> initial_ops = {
        {"put", "old1.lan", 1, 60, {{1, 1, 1, 1}}, 1, 1},
        {"put", "old2.lan", 1, 60, {{1, 1, 1, 2}}, 1, 1},
    };
    REQUIRE(registry.apply_ops(node_a, 1, 5, initial_ops).has_value());
    CHECK(registry.snapshot()->total_record_count() == 2);

    std::vector<Op> snapshot_ops = {
        {"put", "snap1.lan", 1, 120, {{2, 2, 2, 1}}, 2, 2},
    };
    REQUIRE(registry.reset_owner_snapshot(node_a, 1, 10, snapshot_ops).has_value());

    auto snap = registry.snapshot();
    CHECK(snap->total_record_count() == 1);
    auto owner = snap->find_owner(node_a);
    REQUIRE(owner != nullptr);
    CHECK(owner->last_seq == 10);
    CHECK(owner->records.size() == 1);
}

TEST_CASE("Registry online/offline filtering and purge") {
    Registry registry;
    NodeId node_a{};
    node_a[0] = 0xAA;
    NodeId node_b{};
    node_b[0] = 0xBB;

    registry.set_owner_online(node_a, true, "node-a", 100);
    registry.set_owner_online(node_b, false, "node-b", 50);

    std::vector<Op> ops = {
        {"put", "common.lan", 1, 60, {{1, 2, 3, 4}}, 1, 1},
    };
    REQUIRE(registry.apply_ops(node_a, 1, 1, ops).has_value());
    REQUIRE(registry.apply_ops(node_b, 1, 1, ops).has_value());

    auto snap = registry.snapshot();
    auto common_name = dns::Name::from_presentation("common.lan").value();

    // Query with online_only = false returns both
    auto all_claims = snap->find_claims(common_name, 1, false);
    CHECK(all_claims.size() == 2);

    // Query with online_only = true returns only node_a
    auto online_claims = snap->find_claims(common_name, 1, true);
    REQUIRE(online_claims.size() == 1);
    CHECK(online_claims[0].owner_id == node_a);

    // Purge with retention 60s at now_ts = 120
    // node_b last_seen = 50, now_ts - 50 = 70 >= 60 -> purged!
    // node_a is online -> not purged.
    auto purged = registry.purge_offline_owners(std::chrono::seconds(60), 120);
    REQUIRE(purged.size() == 1);
    CHECK(purged[0] == node_b);

    snap = registry.snapshot();
    CHECK(snap->find_owner(node_b) == nullptr);
    CHECK(snap->find_owner(node_a) != nullptr);
    CHECK(snap->total_record_count() == 1);
}

TEST_CASE("Registry init_from_store") {
    Registry registry;

    NodeId self_id{};
    self_id[0] = 0x11;
    NodeId peer_id{};
    peer_id[0] = 0x22;

    std::vector<StoredPeer> peers = {
        {peer_id, "peer-node", 3, 15, "192.168.1.100:5391", 500},
    };

    std::vector<PeerRecordRow> peer_records = {
        {peer_id, "peer.lan", 1, 180, {{192, 168, 1, 100}}, 200},
    };

    RecordRow my_rec;
    my_rec.name = "local.lan";
    my_rec.type = 1;
    my_rec.ttl = 60;
    my_rec.data = {{192, 168, 1, 10}};
    my_rec.created = 100;
    my_rec.updated = 100;

    registry.init_from_store(peers, peer_records, self_id, 2, 7, {my_rec});

    auto snap = registry.snapshot();
    CHECK(snap->total_record_count() == 2);

    auto self_owner = snap->find_owner(self_id);
    REQUIRE(self_owner != nullptr);
    CHECK(self_owner->online == true);
    CHECK(self_owner->epoch == 2);
    CHECK(self_owner->last_seq == 7);

    auto peer_owner = snap->find_owner(peer_id);
    REQUIRE(peer_owner != nullptr);
    CHECK(peer_owner->online == false);
    CHECK(peer_owner->name == "peer-node");
    CHECK(peer_owner->epoch == 3);
    CHECK(peer_owner->last_seq == 15);

    auto local_name = dns::Name::from_presentation("local.lan").value();
    auto peer_name = dns::Name::from_presentation("peer.lan").value();

    CHECK(snap->find_claims(local_name, 1).size() == 1);
    CHECK(snap->find_claims(peer_name, 1).size() == 1);
}
