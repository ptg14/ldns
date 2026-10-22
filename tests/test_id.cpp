#include "doctest.hpp"

#include "core/util/id.hpp"

#include <set>

using namespace ldns;

TEST_CASE("NodeId hex round-trip is lowercase and 32 chars") {
    NodeId id{};
    for (std::size_t i = 0; i < id.size(); ++i) id[i] = static_cast<std::uint8_t>(i * 17 + 3);

    const std::string hex = to_hex(id);
    CHECK(hex.size() == 32);

    NodeId parsed{};
    CHECK(from_hex(hex, parsed));
    CHECK(parsed == id);
}

TEST_CASE("from_hex rejects malformed input") {
    NodeId id{};
    CHECK_FALSE(from_hex("", id));
    CHECK_FALSE(from_hex("zz", id));
    CHECK_FALSE(from_hex("00112233445566778899aabbccddee", id));      // 30 chars
    CHECK_FALSE(from_hex("00112233445566778899aabbccddeeff0", id));   // 33 chars
    CHECK_FALSE(from_hex("00112233445566778899aabbccddeegg", id));    // bad nibble
}

TEST_CASE("from_hex accepts uppercase") {
    NodeId id{};
    CHECK(from_hex("00112233445566778899AABBCCDDEEFF", id));
    CHECK(to_hex(id) == "00112233445566778899aabbccddeeff");
}

TEST_CASE("generate_node_id produces distinct non-zero ids") {
    std::set<std::string> seen;
    for (int i = 0; i < 64; ++i) {
        const NodeId id = generate_node_id();
        CHECK_FALSE(is_zero(id));
        seen.insert(to_hex(id));
    }
    CHECK(seen.size() == 64);
}

TEST_CASE("NodeId ordering is big-endian lexicographic") {
    NodeId a{};
    NodeId b{};
    a[0] = 0x01;
    b[0] = 0x02;
    CHECK(a < b);

    a[0] = 0x02;
    a[15] = 0x00;
    b[0] = 0x02;
    b[15] = 0x01;
    CHECK(a < b);
}
