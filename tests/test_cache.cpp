#include "doctest.hpp"

#include <chrono>
#include <thread>

#include "core/dns/message.hpp"
#include "core/dns/name.hpp"
#include "core/resolver/cache.hpp"

using namespace ldns;
using namespace ldns::dns;
using namespace ldns::resolver;

namespace {

ResourceRecord make_a_record(std::string_view name_str, std::uint32_t ttl,
                             std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d) {
    ResourceRecord rr;
    rr.name = Name::from_presentation(name_str).value();
    rr.type = static_cast<std::uint16_t>(RrType::A);
    rr.rr_class = kClassIn;
    rr.ttl = ttl;
    const std::uint8_t bytes[4] = {a, b, c, d};
    rr.rdata = encode_rdata_a(bytes).value();
    return rr;
}

}  // namespace

TEST_CASE("DnsCache positive caching and TTL decrement") {
    DnsCache cache(10);
    const auto name = Name::from_presentation("example.com").value();
    const auto rr = make_a_record("example.com", 60, 93, 184, 216, 34);

    cache.put_positive(name, static_cast<std::uint16_t>(RrType::A), {rr}, {},
                       CacheSource::Upstream, 300);

    CHECK(cache.size() == 1);

    auto hit = cache.get(name, static_cast<std::uint16_t>(RrType::A));
    REQUIRE(hit.has_value());
    CHECK(hit->rcode == Rcode::NoError);
    CHECK_FALSE(hit->is_negative);

    const auto now = std::chrono::steady_clock::now();
    auto answers = hit->adjusted_answers(now);
    REQUIRE(answers.size() == 1);
    CHECK(answers[0].ttl <= 60);
    CHECK(answers[0].ttl >= 59);
}

TEST_CASE("DnsCache negative caching") {
    DnsCache cache(10);
    const auto name = Name::from_presentation("nonexistent.example.com").value();

    cache.put_negative(name, static_cast<std::uint16_t>(RrType::A),
                       Rcode::NxDomain, std::nullopt, CacheSource::Upstream, 5);

    CHECK(cache.size() == 1);

    auto hit = cache.get(name, static_cast<std::uint16_t>(RrType::A));
    REQUIRE(hit.has_value());
    CHECK(hit->rcode == Rcode::NxDomain);
    CHECK(hit->is_negative);
}

TEST_CASE("DnsCache LRU eviction") {
    DnsCache cache(3);  // max 3 items

    const auto name1 = Name::from_presentation("one.com").value();
    const auto name2 = Name::from_presentation("two.com").value();
    const auto name3 = Name::from_presentation("three.com").value();
    const auto name4 = Name::from_presentation("four.com").value();

    cache.put_positive(name1, 1, {make_a_record("one.com", 60, 1, 1, 1, 1)}, {}, CacheSource::Upstream, 60);
    cache.put_positive(name2, 1, {make_a_record("two.com", 60, 2, 2, 2, 2)}, {}, CacheSource::Upstream, 60);
    cache.put_positive(name3, 1, {make_a_record("three.com", 60, 3, 3, 3, 3)}, {}, CacheSource::Upstream, 60);

    CHECK(cache.size() == 3);

    // Access name1 so it becomes most recently used
    CHECK(cache.get(name1, 1).has_value());

    // Insert name4 -> should evict name2 (least recently used)
    cache.put_positive(name4, 1, {make_a_record("four.com", 60, 4, 4, 4, 4)}, {}, CacheSource::Upstream, 60);

    CHECK(cache.size() == 3);
    CHECK(cache.get(name1, 1).has_value());
    CHECK_FALSE(cache.get(name2, 1).has_value());  // evicted!
    CHECK(cache.get(name3, 1).has_value());
    CHECK(cache.get(name4, 1).has_value());
}

TEST_CASE("DnsCache invalidation by name") {
    DnsCache cache(10);
    const auto name = Name::from_presentation("app.lan").value();

    cache.put_positive(name, 1, {make_a_record("app.lan", 60, 10, 0, 0, 1)}, {}, CacheSource::Peer, 30);
    cache.put_negative(name, 28, Rcode::NoError, std::nullopt, CacheSource::Peer, 5);

    CHECK(cache.size() == 2);

    cache.invalidate(name);
    CHECK(cache.size() == 0);
    CHECK_FALSE(cache.get(name, 1).has_value());
    CHECK_FALSE(cache.get(name, 28).has_value());
}
