#include "doctest.hpp"

#include "core/net/system_dns.hpp"

using namespace ldns::net;

TEST_CASE("System DNS detection") {
    auto res = detect_system_dns();
    REQUIRE(res.has_value());
    // On machines with active internet connections, this returns at least one DNS server.
    // In isolated test environments it may be empty, but must never crash.
    for (const auto& server : res.value().servers) {
        CHECK_FALSE(server.empty());
    }
}
