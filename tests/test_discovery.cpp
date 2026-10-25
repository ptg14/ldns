#include "doctest.hpp"

#include <asio.hpp>
#include <thread>
#include <atomic>

#include "core/net/discovery.hpp"
#include "core/util/id.hpp"

using namespace ldns;
using namespace ldns::net;

TEST_CASE("DiscoveryService sends and receives beacons between nodes") {
    asio::io_context io;

    DiscoveryConfig cfg1;
    cfg1.node_id = generate_node_id();
    cfg1.node_name = "node-alpha";
    cfg1.network_name = "test-net";
    cfg1.discovery_port = 0;  // dynamic port for testing
    cfg1.sync_port = 5391;
    cfg1.dns_port = 53;
    cfg1.multicast_group = ""; // disable multicast for unit test on loopback
    cfg1.enable_broadcast = false;

    DiscoveryConfig cfg2;
    cfg2.node_id = generate_node_id();
    cfg2.node_name = "node-beta";
    cfg2.network_name = "test-net";
    cfg2.discovery_port = 0;
    cfg2.sync_port = 5392;
    cfg2.dns_port = 54;
    cfg2.multicast_group = "";
    cfg2.enable_broadcast = false;

    std::atomic<bool> node1_saw_node2{false};
    std::string discovered_name;

    DiscoveryService ds1(io, cfg1, [&](const DiscoveredPeer& peer) {
        if (peer.node_id == cfg2.node_id) {
            node1_saw_node2 = true;
            discovered_name = peer.name;
        }
    });

    DiscoveryService ds2(io, cfg2, [](const DiscoveredPeer&) {});

    REQUIRE(ds1.start().has_value());
    REQUIRE(ds2.start().has_value());

    const auto port1 = ds1.bound_port();
    const auto port2 = ds2.bound_port();
    REQUIRE(port1 > 0);
    REQUIRE(port2 > 0);

    // Simulate sending beacon from ds2 directly to ds1's port
    Beacon b2;
    b2.type = "beacon";
    b2.version = {1, 0};
    b2.node_id = cfg2.node_id;
    b2.name = cfg2.node_name;
    b2.net = crypto::net_hash(cfg2.network_name);
    b2.sync_port = cfg2.sync_port;
    b2.dns_port = cfg2.dns_port;
    b2.ips = {{127, 0, 0, 1}};

    auto enc = encode_beacon(b2);
    REQUIRE(enc.has_value());

    asio::ip::udp::socket sender(io);
    sender.open(asio::ip::udp::v4());
    asio::ip::udp::endpoint target(asio::ip::address_v4::loopback(), port1);
    sender.send_to(asio::buffer(enc.value()), target);

    // Poll io context
    for (int i = 0; i < 50 && !node1_saw_node2; ++i) {
        io.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    CHECK(node1_saw_node2);
    CHECK(discovered_name == "node-beta");

    ds1.stop();
    ds2.stop();
}
