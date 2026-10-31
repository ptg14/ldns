#include "core/resolver/peer_ranking.hpp"

#include <algorithm>
#include <random>
#include <vector>

#include "doctest.hpp"
#include "core/dns/message.hpp"

using namespace ldns;
using namespace ldns::resolver;

static NodeId make_node_id(std::uint8_t byte_val) {
    NodeId id{};
    id.fill(byte_val);
    return id;
}

TEST_CASE("canonicalize_rrset_data sorts and deduplicates") {
    std::vector<std::vector<std::uint8_t>> raw = {
        {192, 168, 1, 100},
        {10, 0, 0, 1},
        {192, 168, 1, 100},
        {172, 16, 0, 1}
    };

    auto canon = canonicalize_rrset_data(raw);
    REQUIRE(canon.size() == 3);
    CHECK(canon[0] == std::vector<std::uint8_t>{10, 0, 0, 1});
    CHECK(canon[1] == std::vector<std::uint8_t>{172, 16, 0, 1});
    CHECK(canon[2] == std::vector<std::uint8_t>{192, 168, 1, 100});
}

TEST_CASE("rank_peer_claims deterministic ranking rules") {
    // Construct registry with 3 owners
    // Node A (id 0x01)
    // Node B (id 0x02)
    // Node C (id 0x03)
    auto id_a = make_node_id(0x01);
    auto id_b = make_node_id(0x02);
    auto id_c = make_node_id(0x03);

    Registry registry;

    // Rule 1: Vote count wins
    // A and B vote for 192.168.1.10
    // C votes for 192.168.1.99
    registry.set_owner_online(id_a, true, "node-a", 0, 1, 0);
    registry.set_owner_online(id_b, true, "node-b", 0, 1, 0);
    registry.set_owner_online(id_c, true, "node-c", 0, 1, 0);

    net::Op op_a;
    op_a.op = "put";
    op_a.name = "app.lan";
    op_a.type = 1; // A
    op_a.ttl = 60;
    op_a.data = {{192, 168, 1, 10}};
    op_a.created = 1000;
    op_a.ts = 1000;

    net::Op op_b = op_a;
    op_b.created = 1500;
    op_b.ts = 1500;

    net::Op op_c;
    op_c.op = "put";
    op_c.name = "app.lan";
    op_c.type = 1;
    op_c.ttl = 30;
    op_c.data = {{192, 168, 1, 99}};
    op_c.created = 500; // Older than A and B, but has fewer votes!
    op_c.ts = 500;

    REQUIRE(registry.apply_ops(id_a, 1, 10, {op_a}).has_value());
    REQUIRE(registry.apply_ops(id_b, 1, 10, {op_b}).has_value());
    REQUIRE(registry.apply_ops(id_c, 1, 10, {op_c}).has_value());

    auto snap = registry.snapshot();
    auto qname = dns::Name::from_presentation("app.lan").value();

    SUBCASE("Vote count takes precedence over timestamp") {
        auto res = rank_peer_claims(*snap, qname, 1);
        REQUIRE(res.groups.size() == 2);
        // Group 0 has 2 votes (Node A + B)
        CHECK(res.groups[0].vote_count == 2);
        CHECK(res.groups[0].canonical_data[0] == std::vector<std::uint8_t>{192, 168, 1, 10});
        CHECK(res.groups[0].candidates.size() == 2);

        // Group 1 has 1 vote (Node C)
        CHECK(res.groups[1].vote_count == 1);
        CHECK(res.groups[1].canonical_data[0] == std::vector<std::uint8_t>{192, 168, 1, 99});

        // Conflict is recorded
        REQUIRE(res.conflict.has_value());
        CHECK(res.conflict->groups.size() == 2);
    }

    SUBCASE("Tie in votes broken by oldest claim timestamp") {
        Registry reg2;
        reg2.set_owner_online(id_a, true, "node-a", 0, 1, 0);
        reg2.set_owner_online(id_b, true, "node-b", 0, 1, 0);

        // Node A: created at 2000
        net::Op oa = op_a;
        oa.created = 2000;

        // Node B: created at 1000 (older)
        net::Op ob = op_c;
        ob.created = 1000;

        REQUIRE(reg2.apply_ops(id_a, 1, 10, {oa}).has_value());
        REQUIRE(reg2.apply_ops(id_b, 1, 10, {ob}).has_value());

        auto snap2 = reg2.snapshot();
        auto res2 = rank_peer_claims(*snap2, qname, 1);
        REQUIRE(res2.groups.size() == 2);
        CHECK(res2.groups[0].vote_count == 1);
        CHECK(res2.groups[1].vote_count == 1);
        // Node B's group wins because 1000 < 2000
        CHECK(res2.groups[0].canonical_data[0] == std::vector<std::uint8_t>{192, 168, 1, 99});
        CHECK(res2.groups[0].oldest_claim_created == 1000);
        CHECK(res2.groups[1].canonical_data[0] == std::vector<std::uint8_t>{192, 168, 1, 10});
        CHECK(res2.groups[1].oldest_claim_created == 2000);
    }

    SUBCASE("Tie in votes and timestamp broken by lowest node_id") {
        Registry reg3;
        reg3.set_owner_online(id_a, true, "node-a", 0, 1, 0);
        reg3.set_owner_online(id_b, true, "node-b", 0, 1, 0);

        net::Op oa = op_a;
        oa.created = 1000;

        net::Op ob = op_c;
        ob.created = 1000; // Same created timestamp!

        REQUIRE(reg3.apply_ops(id_a, 1, 10, {oa}).has_value());
        REQUIRE(reg3.apply_ops(id_b, 1, 10, {ob}).has_value());

        auto snap3 = reg3.snapshot();
        auto res3 = rank_peer_claims(*snap3, qname, 1);
        REQUIRE(res3.groups.size() == 2);
        // id_a (0x01) < id_b (0x02) -> Node A's group wins
        CHECK(res3.groups[0].canonical_data[0] == std::vector<std::uint8_t>{192, 168, 1, 10});
        CHECK(res3.groups[0].lowest_node_id == id_a);
        CHECK(res3.groups[1].canonical_data[0] == std::vector<std::uint8_t>{192, 168, 1, 99});
        CHECK(res3.groups[1].lowest_node_id == id_b);
    }
}

TEST_CASE("rank_peer_claims wildcard and CNAME fallbacks") {
    auto id_a = make_node_id(0x01);
    auto id_b = make_node_id(0x02);

    Registry reg;
    reg.set_owner_online(id_a, true, "node-a", 0, 1, 0);
    reg.set_owner_online(id_b, true, "node-b", 0, 1, 0);

    // Node A owns *.lan (wildcard)
    net::Op op_wild;
    op_wild.op = "put";
    op_wild.name = "*.lan";
    op_wild.type = 1;
    op_wild.ttl = 60;
    op_wild.data = {{10, 0, 0, 1}};
    op_wild.created = 100;
    op_wild.ts = 100;
    REQUIRE(reg.apply_ops(id_a, 1, 10, {op_wild}).has_value());

    // Node B owns specific sub.app.lan (exact)
    net::Op op_exact;
    op_exact.op = "put";
    op_exact.name = "sub.app.lan";
    op_exact.type = 1;
    op_exact.ttl = 60;
    op_exact.data = {{10, 0, 0, 2}};
    op_exact.created = 200;
    op_exact.ts = 200;
    REQUIRE(reg.apply_ops(id_b, 1, 10, {op_exact}).has_value());

    auto snap = reg.snapshot();

    // Query sub.app.lan: Node B has exact match, Node A has wildcard match
    auto q_sub = dns::Name::from_presentation("sub.app.lan").value();
    auto res_sub = rank_peer_claims(*snap, q_sub, 1);
    REQUIRE(res_sub.groups.size() == 2);

    // Query other.lan: Only Node A wildcard matches
    auto q_other = dns::Name::from_presentation("other.lan").value();
    auto res_other = rank_peer_claims(*snap, q_other, 1);
    REQUIRE(res_other.groups.size() == 1);
    CHECK(res_other.groups[0].canonical_data[0] == std::vector<std::uint8_t>{10, 0, 0, 1});
    CHECK(res_other.groups[0].candidates[0].node_id == id_a);

    // CNAME fallback: Node B has CNAME for alias.lan
    net::Op op_cname;
    op_cname.op = "put";
    op_cname.name = "alias.lan";
    op_cname.type = 5; // CNAME
    op_cname.ttl = 120;
    auto target_name = dns::Name::from_presentation("target.lan").value();
    auto target_wire = target_name.wire();
    op_cname.data = {std::vector<std::uint8_t>(target_wire.begin(), target_wire.end())};
    op_cname.created = 300;
    op_cname.ts = 300;
    REQUIRE(reg.apply_ops(id_b, 1, 11, {op_cname}).has_value());

    auto snap2 = reg.snapshot();
    auto q_alias = dns::Name::from_presentation("alias.lan").value();
    // Querying type A (1) for alias.lan: should return CNAME (5) group!
    auto res_cname = rank_peer_claims(*snap2, q_alias, 1);
    REQUIRE(!res_cname.groups.empty());
    bool found_cname = false;
    for (const auto& g : res_cname.groups) {
        if (g.type == 5) {
            found_cname = true;
            CHECK(g.candidates[0].node_id == id_b);
        }
    }
    CHECK(found_cname);
}

TEST_CASE("rank_peer_claims orders candidates by RTT") {
    auto id_a = make_node_id(0x01);
    auto id_b = make_node_id(0x02);

    Registry reg;
    reg.set_owner_online(id_a, true, "node-a", 0, 1, 0);
    reg.set_owner_online(id_b, true, "node-b", 0, 1, 0);

    net::Op op;
    op.op = "put";
    op.name = "service.lan";
    op.type = 1;
    op.ttl = 60;
    op.data = {{10, 0, 0, 5}};
    op.created = 100;
    op.ts = 100;

    REQUIRE(reg.apply_ops(id_a, 1, 10, {op}).has_value());
    REQUIRE(reg.apply_ops(id_b, 1, 10, {op}).has_value());

    auto snap = reg.snapshot();
    auto qname = dns::Name::from_presentation("service.lan").value();

    // Node A RTT = 50ms, Node B RTT = 5ms -> Node B should be first candidate!
    std::unordered_map<NodeId, std::uint64_t> rtt_map;
    rtt_map[id_a] = 50;
    rtt_map[id_b] = 5;

    auto res = rank_peer_claims(*snap, qname, 1, true, rtt_map);
    REQUIRE(res.groups.size() == 1);
    REQUIRE(res.groups[0].candidates.size() == 2);
    CHECK(res.groups[0].candidates[0].node_id == id_b);
    CHECK(res.groups[0].candidates[0].rtt_ms == 5);
    CHECK(res.groups[0].candidates[1].node_id == id_a);
    CHECK(res.groups[0].candidates[1].rtt_ms == 50);
}
