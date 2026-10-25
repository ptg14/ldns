#include "doctest.hpp"

#include "core/net/protocol_message.hpp"

using namespace ldns;
using namespace ldns::net;

TEST_CASE("Protocol framing adds 4-byte BE length prefix") {
    std::vector<std::uint8_t> payload = {1, 2, 3, 4, 5};
    auto framed = frame_payload(payload);
    REQUIRE(framed.has_value());
    REQUIRE(framed.value().size() == 9);

    CHECK(framed.value()[0] == 0);
    CHECK(framed.value()[1] == 0);
    CHECK(framed.value()[2] == 0);
    CHECK(framed.value()[3] == 5);
    for (std::size_t i = 0; i < 5; ++i) {
        CHECK(framed.value()[4 + i] == payload[i]);
    }
}

TEST_CASE("Protocol HELLO message encode and parse roundtrip") {
    HelloMessage h;
    h.version = {1, 0};
    for (std::size_t i = 0; i < 16; ++i) h.node_id[i] = static_cast<std::uint8_t>(i + 10);
    h.name = "peer-one";
    h.net = {0xaa, 0xbb, 0xcc, 0xdd, 0x11, 0x22, 0x33, 0x44};
    h.epoch = 999;
    h.seq = 77;
    h.sync_port = 5391;
    h.dns_port = 53;
    h.ips = {{192, 168, 1, 10}};
    h.caps = {"relay", "snapshot-chunk"};
    h.nonce = {0};
    h.psk = false;

    auto frame = encode_hello_frame(h);
    REQUIRE(frame.has_value());
    REQUIRE(frame.value().size() > 4);

    // Extract payload by skipping 4-byte length prefix
    std::span<const std::uint8_t> payload(frame.value().data() + 4, frame.value().size() - 4);
    auto parsed = parse_payload(payload);
    REQUIRE(parsed.has_value());
    CHECK(parsed.value().type == MessageType::Hello);
    REQUIRE(parsed.value().hello.has_value());

    const auto& res = parsed.value().hello.value();
    CHECK(res.name == "peer-one");
    CHECK(res.node_id == h.node_id);
    CHECK(res.net == h.net);
    CHECK(res.epoch == 999);
    CHECK(res.seq == 77);
    CHECK(res.sync_port == 5391);
    CHECK(res.dns_port == 53);
    REQUIRE(res.ips.size() == 1);
    CHECK(res.ips[0] == std::vector<std::uint8_t>{192, 168, 1, 10});
}

TEST_CASE("Protocol PING, PONG, BYE, and ERROR messages") {
    // PING
    auto ping_frame = encode_ping_frame();
    REQUIRE(ping_frame.has_value());
    std::span<const std::uint8_t> ping_pld(ping_frame.value().data() + 4, ping_frame.value().size() - 4);
    auto ping_parsed = parse_payload(ping_pld);
    REQUIRE(ping_parsed.has_value());
    CHECK(ping_parsed.value().type == MessageType::Ping);

    // PONG
    auto pong_frame = encode_pong_frame();
    REQUIRE(pong_frame.has_value());
    std::span<const std::uint8_t> pong_pld(pong_frame.value().data() + 4, pong_frame.value().size() - 4);
    auto pong_parsed = parse_payload(pong_pld);
    REQUIRE(pong_parsed.has_value());
    CHECK(pong_parsed.value().type == MessageType::Pong);

    // BYE
    auto bye_frame = encode_bye_frame();
    REQUIRE(bye_frame.has_value());
    std::span<const std::uint8_t> bye_pld(bye_frame.value().data() + 4, bye_frame.value().size() - 4);
    auto bye_parsed = parse_payload(bye_pld);
    REQUIRE(bye_parsed.has_value());
    CHECK(bye_parsed.value().type == MessageType::Bye);

    // ERROR
    ErrorMessage err{ProtocolErrorCode::Duplicate, "connection already established"};
    auto err_frame = encode_error_frame(err);
    REQUIRE(err_frame.has_value());
    std::span<const std::uint8_t> err_pld(err_frame.value().data() + 4, err_frame.value().size() - 4);
    auto err_parsed = parse_payload(err_pld);
    REQUIRE(err_parsed.has_value());
    CHECK(err_parsed.value().type == MessageType::Error);
    REQUIRE(err_parsed.value().error.has_value());
    CHECK(err_parsed.value().error->code == ProtocolErrorCode::Duplicate);
    CHECK(err_parsed.value().error->msg == "connection already established");
}

TEST_CASE("Protocol PEERS exchange message") {
    PeersMessage pm;
    PeerItem item;
    for (std::size_t i = 0; i < 16; ++i) item.node_id[i] = static_cast<std::uint8_t>(i + 5);
    item.sync_port = 5391;
    item.ips = {{10, 0, 0, 5}};
    pm.peers.push_back(item);

    auto peers_frame = encode_peers_frame(pm);
    REQUIRE(peers_frame.has_value());
    std::span<const std::uint8_t> peers_pld(peers_frame.value().data() + 4, peers_frame.value().size() - 4);
    auto peers_parsed = parse_payload(peers_pld);
    REQUIRE(peers_parsed.has_value());
    CHECK(peers_parsed.value().type == MessageType::Peers);
    REQUIRE(peers_parsed.value().peers.has_value());
    REQUIRE(peers_parsed.value().peers->peers.size() == 1);
    CHECK(peers_parsed.value().peers->peers[0].sync_port == 5391);
    CHECK(peers_parsed.value().peers->peers[0].node_id == item.node_id);
}
