#include "doctest.hpp"

#include "core/net/beacon.hpp"
#include "core/net/crypto.hpp"

using namespace ldns;
using namespace ldns::net;

TEST_CASE("Beacon encode and decode roundtrip") {
    Beacon b;
    b.type = "beacon";
    b.version = {1, 0};
    for (std::size_t i = 0; i < 16; ++i) b.node_id[i] = static_cast<std::uint8_t>(i + 1);
    b.name = "laptop-dthan";
    b.net = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    b.sync_port = 5391;
    b.dns_port = 53;
    b.ips = {{192, 168, 1, 50}, {10, 0, 0, 1}};
    b.epoch = 12345;
    b.seq = 42;
    b.ts = 999999999;

    auto encoded = encode_beacon(b);
    REQUIRE(encoded.has_value());
    CHECK(encoded.value().size() > 5);
    CHECK(encoded.value().size() <= kMaxBeaconDatagramSize);

    auto decoded = decode_beacon(encoded.value());
    REQUIRE(decoded.has_value());
    CHECK(decoded.value().type == "beacon");
    CHECK(decoded.value().version[0] == 1);
    CHECK(decoded.value().version[1] == 0);
    CHECK(decoded.value().node_id == b.node_id);
    CHECK(decoded.value().name == "laptop-dthan");
    CHECK(decoded.value().net == b.net);
    CHECK(decoded.value().sync_port == 5391);
    CHECK(decoded.value().dns_port == 53);
    REQUIRE(decoded.value().ips.size() == 2);
    CHECK(decoded.value().ips[0] == std::vector<std::uint8_t>{192, 168, 1, 50});
    CHECK(decoded.value().ips[1] == std::vector<std::uint8_t>{10, 0, 0, 1});
    CHECK(decoded.value().epoch == 12345);
    CHECK(decoded.value().seq == 42);
    CHECK(decoded.value().ts == 999999999);
}

TEST_CASE("Beacon rejects bad magic or truncated datagram") {
    std::vector<std::uint8_t> short_pkt = {'L', 'D', 'B'};
    CHECK_FALSE(decode_beacon(short_pkt).has_value());

    std::vector<std::uint8_t> bad_magic = {'X', 'X', 'X', 'X', 0x00, 0x80};
    CHECK(decode_beacon(bad_magic).error().code == 805);

    std::vector<std::uint8_t> oversize(1201, 0);
    CHECK(decode_beacon(oversize).error().code == 803);
}

TEST_CASE("Crypto helper SHA256 and net_hash") {
    auto init_res = crypto::init();
    REQUIRE(init_res.has_value());

    auto hash1 = crypto::net_hash("office");
    auto hash2 = crypto::net_hash("office");
    auto hash3 = crypto::net_hash("home");

    CHECK(hash1 == hash2);
    CHECK(hash1 != hash3);

    auto rnd = crypto::random_bytes_32();
    bool all_zero = true;
    for (auto b : rnd) {
        if (b != 0) all_zero = false;
    }
    CHECK_FALSE(all_zero);
}
