#include "doctest.hpp"

#include <memory>

#include "core/dns/local_zone.hpp"
#include "core/dns/message.hpp"
#include "core/dns/name.hpp"
#include "core/resolver/cache.hpp"
#include "core/resolver/pipeline.hpp"
#include "core/resolver/query_log.hpp"
#include "core/resolver/upstream_client.hpp"

using namespace ldns;
using namespace ldns::dns;
using namespace ldns::resolver;

namespace {

ResourceRecord make_a_rr(std::string_view name_str, std::uint32_t ttl,
                         std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d) {
    ResourceRecord rr;
    rr.name = Name::from_presentation(name_str).value();
    rr.type = static_cast<std::uint16_t>(RrType::A);
    rr.rr_class = kClassIn;
    rr.ttl = ttl;
    const std::uint8_t ip[4] = {a, b, c, d};
    rr.rdata = encode_rdata_a(ip).value();
    return rr;
}

Message make_query(std::string_view name_str, std::uint16_t type) {
    Message msg;
    msg.id = 1001;
    msg.flags = 0;
    msg.questions.push_back(Question{Name::from_presentation(name_str).value(), type, kClassIn});
    return msg;
}

}  // namespace

TEST_CASE("ResolverPipeline Step 1 LocalZone hit") {
    LocalZone zone;
    const auto name = Name::from_presentation("myhost.lan").value();
    const std::uint8_t ip[4] = {192, 168, 1, 50};
    REQUIRE(zone.upsert(name, 1, 60, {encode_rdata_a(ip).value()}).has_value());

    auto cache = std::make_shared<DnsCache>();
    auto query_log = std::make_shared<QueryLog>();
    ResolverPipeline pipeline(zone, cache, nullptr, query_log);

    auto res = pipeline.resolve(make_query("myhost.lan", 1));
    CHECK(res.response.rcode() == static_cast<std::uint8_t>(Rcode::NoError));
    CHECK((res.response.flags & kAa) != 0);  // AA = 1
    CHECK(res.resolved_step == ResolutionStep::Local);
    REQUIRE(res.response.answers.size() == 1);
    CHECK(res.response.answers[0].rdata[0] == 192);

    CHECK(query_log->size() == 1);
}

TEST_CASE("ResolverPipeline LocalZone NODATA stops without querying upstream") {
    LocalZone zone;
    const auto name = Name::from_presentation("only-a.lan").value();
    const std::uint8_t ip[4] = {10, 0, 0, 1};
    REQUIRE(zone.upsert(name, 1, 60, {encode_rdata_a(ip).value()}).has_value());

    auto cache = std::make_shared<DnsCache>();
    auto query_log = std::make_shared<QueryLog>();

    // Upstream client points to non-existent server
    UpstreamConfig up_config;
    up_config.servers = {"192.0.2.1:53"};  // TEST-NET-1 (unreachable)
    up_config.timeout_ms = 50;
    up_config.retries = 0;
    auto upstream = std::make_shared<UpstreamClient>(up_config);

    ResolverPipeline pipeline(zone, cache, upstream, query_log);

    // Query for AAAA on a name that only has A
    auto res = pipeline.resolve(make_query("only-a.lan", 28 /* AAAA */));
    CHECK(res.response.rcode() == static_cast<std::uint8_t>(Rcode::NoError));
    CHECK((res.response.flags & kAa) != 0);  // AA = 1
    CHECK(res.resolved_step == ResolutionStep::Local);
    CHECK(res.response.answers.empty());  // NODATA
}

TEST_CASE("ResolverPipeline Cache hit on second query") {
    LocalZone zone;
    auto cache = std::make_shared<DnsCache>();
    auto query_log = std::make_shared<QueryLog>();

    const auto name = Name::from_presentation("cached.com").value();
    cache->put_positive(name, 1, {make_a_rr("cached.com", 60, 1, 1, 1, 1)}, {},
                       CacheSource::Upstream, 300);

    ResolverPipeline pipeline(zone, cache, nullptr, query_log);

    auto res = pipeline.resolve(make_query("cached.com", 1));
    CHECK(res.response.rcode() == static_cast<std::uint8_t>(Rcode::NoError));
    CHECK(res.resolved_step == ResolutionStep::Cache);
    CHECK(res.from_cache);
    REQUIRE(res.response.answers.size() == 1);
}

TEST_CASE("ResolverPipeline Trace records hops") {
    LocalZone zone;
    const auto name = Name::from_presentation("trace.lan").value();
    const std::uint8_t ip[4] = {192, 168, 1, 100};
    REQUIRE(zone.upsert(name, 1, 60, {encode_rdata_a(ip).value()}).has_value());

    auto cache = std::make_shared<DnsCache>();
    ResolverPipeline pipeline(zone, cache, nullptr, nullptr);

    auto res = pipeline.resolve(make_query("trace.lan", 1), QueryContext{}, true /* trace */);
    REQUIRE(res.trace.size() >= 1);
    CHECK(res.trace[0].step == ResolutionStep::Local);
    CHECK(res.trace[0].qname.to_string() == "trace.lan");
    CHECK(res.trace[0].rcode == Rcode::NoError);
}
