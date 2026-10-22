#include "doctest.hpp"

#include "core/server/acl.hpp"

#include <string>

using namespace ldns;

namespace {
IpAddress ip(const std::string& text) { return IpAddress::parse(text).value(); }
}  // namespace

TEST_CASE("IpAddress parses and formats both families") {
    CHECK(ip("192.168.1.10").to_string() == "192.168.1.10");
    CHECK(ip("::1").to_string() == "::1");
    CHECK(ip("fe80::1").v6);
    CHECK_FALSE(ip("10.0.0.1").v6);
    CHECK_FALSE(IpAddress::parse("not-an-ip").has_value());
}

TEST_CASE("CIDR parsing and matching") {
    const auto lan = Cidr::parse("192.168.1.0/24").value();
    CHECK(lan.contains(ip("192.168.1.1")));
    CHECK(lan.contains(ip("192.168.1.255")));
    CHECK_FALSE(lan.contains(ip("192.168.2.1")));
    CHECK_FALSE(lan.contains(ip("::1")));

    const auto v6 = Cidr::parse("2001:db8::/32").value();
    CHECK(v6.contains(ip("2001:db8::1")));
    CHECK_FALSE(v6.contains(ip("2001:db9::1")));

    // Host bits in the input are masked off.
    const auto odd = Cidr::parse("10.1.2.3/8").value();
    CHECK(odd.contains(ip("10.200.0.1")));

    CHECK(Cidr::parse("10.0.0.0/33").has_value() == false);
    CHECK(Cidr::parse("10.0.0.0/abc").has_value() == false);
    CHECK(Cidr::parse("bogus/24").has_value() == false);
}

TEST_CASE("default ACL serves private sources only") {
    const Acl acl;
    CHECK(acl.permits(ip("192.168.1.10")));
    CHECK(acl.permits(ip("10.1.2.3")));
    CHECK(acl.permits(ip("172.16.5.5")));
    CHECK(acl.permits(ip("127.0.0.1")));
    CHECK(acl.permits(ip("169.254.1.1")));
    CHECK(acl.permits(ip("fe80::1")));
    CHECK(acl.permits(ip("fd00::1")));
    CHECK(acl.permits(ip("::1")));

    CHECK_FALSE(acl.permits(ip("8.8.8.8")));
    CHECK_FALSE(acl.permits(ip("1.1.1.1")));
    CHECK_FALSE(acl.permits(ip("2001:4860:4860::8888")));
}

TEST_CASE("explicit allow opens a public source and is narrower than deny") {
    Acl acl;
    REQUIRE(acl.allow("203.0.113.0/24").has_value());
    CHECK(acl.permits(ip("203.0.113.5")));
    CHECK_FALSE(acl.permits(ip("203.0.114.5")));

    REQUIRE(acl.deny("203.0.113.9/32").has_value());
    CHECK_FALSE(acl.permits(ip("203.0.113.9")));
    CHECK(acl.permits(ip("203.0.113.10")));
}

TEST_CASE("ACL without private defaults refuses everything not explicitly allowed") {
    Acl acl(false);
    CHECK_FALSE(acl.permits(ip("192.168.1.10")));
    CHECK(acl.rule_count() == 0);
    REQUIRE(acl.allow("192.168.0.0/16").has_value());
    CHECK(acl.permits(ip("192.168.1.10")));
}

TEST_CASE("rate limiter enforces burst then refills") {
    RateLimiter limiter(100, 10);  // 100 qps, burst 10
    const IpAddress source = ip("192.168.1.10");
    const std::int64_t start = 1'000'000'000;

    for (int i = 0; i < 10; ++i) {
        CHECK(limiter.allow(source, start));
    }
    CHECK_FALSE(limiter.allow(source, start));

    // After 50 ms at 100 qps, 5 tokens are available.
    const std::int64_t later = start + 50'000'000;
    for (int i = 0; i < 5; ++i) {
        CHECK(limiter.allow(source, later));
    }
    CHECK_FALSE(limiter.allow(source, later));

    // Tokens never exceed the burst size.
    const std::int64_t much_later = later + 10'000'000'000;
    for (int i = 0; i < 10; ++i) {
        CHECK(limiter.allow(source, much_later));
    }
    CHECK_FALSE(limiter.allow(source, much_later));
}

TEST_CASE("a first query at t=0 still gets a full burst") {
    // Regression: buckets used last_ns == 0 as the "new bucket" sentinel, so a
    // source whose first query arrived at now_ns == 0 received no tokens.
    RateLimiter limiter(100, 3);
    const IpAddress source = ip("10.0.0.7");
    CHECK(limiter.allow(source, 0));
    CHECK(limiter.allow(source, 0));
    CHECK(limiter.allow(source, 0));
    CHECK_FALSE(limiter.allow(source, 0));
}

TEST_CASE("rate limiter buckets are per source") {
    RateLimiter limiter(100, 2);
    const std::int64_t now = 5'000'000'000;
    CHECK(limiter.allow(ip("10.0.0.1"), now));
    CHECK(limiter.allow(ip("10.0.0.1"), now));
    CHECK_FALSE(limiter.allow(ip("10.0.0.1"), now));
    CHECK(limiter.allow(ip("10.0.0.2"), now));
    CHECK(limiter.tracked_sources() == 2);

    limiter.clear();
    CHECK(limiter.tracked_sources() == 0);
    CHECK(limiter.allow(ip("10.0.0.1"), now));
}
