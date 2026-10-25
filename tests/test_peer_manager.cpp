#include "doctest.hpp"

#include <asio.hpp>
#include <atomic>
#include <thread>

#include "core/net/peer_manager.hpp"
#include "core/util/id.hpp"

using namespace ldns;
using namespace ldns::net;

TEST_CASE("PeerManager dials smaller node_id and establishes connection") {
    asio::io_context io;

    // Node 1 has smaller node_id
    NodeId id1{};
    id1.fill(0x01);

    // Node 2 has larger node_id
    NodeId id2{};
    id2.fill(0x02);

    CHECK(id1 < id2);

    PeerManagerConfig cfg1;
    cfg1.node_id = id1;
    cfg1.node_name = "node-one";
    cfg1.network_name = "test-net";
    cfg1.sync_port = 0; // dynamic port
    cfg1.dns_port = 53;

    PeerManagerConfig cfg2;
    cfg2.node_id = id2;
    cfg2.node_name = "node-two";
    cfg2.network_name = "test-net";
    cfg2.sync_port = 0;
    cfg2.dns_port = 53;

    std::atomic<bool> node1_peer_up{false};
    std::atomic<bool> node2_peer_up{false};

    auto pm1 = PeerManager::create(io, cfg1, [&](const PeerRecord&, bool online) {
        if (online) node1_peer_up = true;
    });

    auto pm2 = PeerManager::create(io, cfg2, [&](const PeerRecord&, bool online) {
        if (online) node2_peer_up = true;
    });

    REQUIRE(pm1->start().has_value());
    REQUIRE(pm2->start().has_value());

    const auto port1 = pm1->bound_port();
    const auto port2 = pm2->bound_port();
    REQUIRE(port1 > 0);
    REQUIRE(port2 > 0);

    // Simulate Node 1 receiving beacon from Node 2
    DiscoveredPeer p2;
    p2.node_id = id2;
    p2.name = "node-two";
    p2.sync_port = port2;
    p2.dns_port = 53;
    p2.source_endpoint = asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), port2);
    p2.ips = {"127.0.0.1"};

    // Node 1 receives beacon: because id1 < id2, Node 1 will dial Node 2!
    pm1->on_beacon_discovered(p2);

    for (int i = 0; i < 50 && (!node1_peer_up || !node2_peer_up); ++i) {
        io.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    CHECK(node1_peer_up);
    CHECK(node2_peer_up);

    CHECK(pm1->online_peer_count() == 1);
    CHECK(pm2->online_peer_count() == 1);

    auto peer_rec = pm1->get_peer(id2);
    REQUIRE(peer_rec.has_value());
    CHECK(peer_rec->name == "node-two");
    CHECK(peer_rec->is_online);

    pm1->stop();
    pm2->stop();
}

TEST_CASE("PeerManager manual peer management") {
    asio::io_context io;

    PeerManagerConfig cfg;
    cfg.node_id = generate_node_id();
    cfg.node_name = "node-local";
    cfg.network_name = "test-net";
    cfg.sync_port = 0;

    auto pm = PeerManager::create(io, cfg);
    REQUIRE(pm->start().has_value());

    REQUIRE(pm->add_manual_peer("192.168.1.100:5391").has_value());
    auto list = pm->list_peers();
    REQUIRE(list.size() == 1);
    CHECK(list[0].is_manual);

    REQUIRE(pm->remove_manual_peer("192.168.1.100:5391").has_value());
    CHECK(pm->list_peers().empty());

    pm->stop();
}
