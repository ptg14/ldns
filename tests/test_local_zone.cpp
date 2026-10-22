#include "doctest.hpp"

#include "core/dns/local_zone.hpp"

#include <string>
#include <vector>

using namespace ldns;
using namespace ldns::dns;

namespace {

Name nm(const std::string& text) { return Name::from_presentation(text).value(); }

std::vector<std::uint8_t> a(const std::string& ip) { return parse_ipv4(ip).value(); }

void put_a(LocalZone& zone, const std::string& name, const std::string& ip,
           std::uint32_t ttl = 60) {
    auto status = zone.upsert(nm(name), static_cast<std::uint16_t>(RrType::A), ttl, {a(ip)});
    REQUIRE(status.has_value());
}

}  // namespace

TEST_CASE("exact A lookup answers with AA-ready records") {
    LocalZone zone;
    put_a(zone, "app.lan", "192.168.1.10");

    const auto outcome = zone.lookup(nm("app.lan"), static_cast<std::uint16_t>(RrType::A));
    CHECK(outcome.kind == LookupKind::Answer);
    REQUIRE(outcome.answers.size() == 1);
    CHECK(outcome.answers[0].name == nm("app.lan"));
    CHECK(outcome.answers[0].ttl == 60);
    CHECK(format_ipv4(outcome.answers[0].rdata) == "192.168.1.10");
}

TEST_CASE("lookup is case-insensitive thanks to normalization") {
    LocalZone zone;
    put_a(zone, "app.lan", "10.0.0.1");
    const auto outcome = zone.lookup(nm("APP.LAN"), static_cast<std::uint16_t>(RrType::A));
    CHECK(outcome.kind == LookupKind::Answer);
}

TEST_CASE("multiple values form one RRset") {
    LocalZone zone;
    auto status = zone.upsert(nm("app.lan"), static_cast<std::uint16_t>(RrType::A), 30,
                              {a("10.0.0.1"), a("10.0.0.2")});
    REQUIRE(status.has_value());

    const auto outcome = zone.lookup(nm("app.lan"), static_cast<std::uint16_t>(RrType::A));
    CHECK(outcome.kind == LookupKind::Answer);
    CHECK(outcome.answers.size() == 2);
    CHECK(outcome.answers[0].ttl == 30);
}

TEST_CASE("NODATA when the name exists with another type") {
    LocalZone zone;
    put_a(zone, "app.lan", "10.0.0.1");

    const auto outcome = zone.lookup(nm("app.lan"), static_cast<std::uint16_t>(RrType::AAAA));
    CHECK(outcome.kind == LookupKind::NoData);
    CHECK(outcome.answers.empty());
}

TEST_CASE("NXDOMAIN when the name does not exist") {
    LocalZone zone;
    put_a(zone, "app.lan", "10.0.0.1");
    const auto outcome = zone.lookup(nm("nope.lan"), static_cast<std::uint16_t>(RrType::A));
    CHECK(outcome.kind == LookupKind::NxDomain);
}

TEST_CASE("wildcard covers descendants, exact names win") {
    LocalZone zone;
    auto status = zone.upsert(nm("*.app.lan"), static_cast<std::uint16_t>(RrType::A), 60,
                              {a("10.0.0.9")});
    REQUIRE(status.has_value());
    put_a(zone, "db.app.lan", "10.0.0.10");

    const auto wild = zone.lookup(nm("x.app.lan"), static_cast<std::uint16_t>(RrType::A));
    CHECK(wild.kind == LookupKind::Answer);
    REQUIRE(wild.answers.size() == 1);
    CHECK(wild.answers[0].name == nm("x.app.lan"));  // owner is the queried name
    CHECK(format_ipv4(wild.answers[0].rdata) == "10.0.0.9");

    const auto exact = zone.lookup(nm("db.app.lan"), static_cast<std::uint16_t>(RrType::A));
    CHECK(exact.kind == LookupKind::Answer);
    CHECK(format_ipv4(exact.answers[0].rdata) == "10.0.0.10");

    // Deeper names still match the wildcard (RFC 1034 §4.3.2).
    const auto deep = zone.lookup(nm("a.b.app.lan"), static_cast<std::uint16_t>(RrType::A));
    CHECK(deep.kind == LookupKind::Answer);
}

TEST_CASE("most specific wildcard wins") {
    LocalZone zone;
    REQUIRE(zone.upsert(nm("*.lan"), static_cast<std::uint16_t>(RrType::A), 60, {a("10.0.0.1")})
                .has_value());
    REQUIRE(zone.upsert(nm("*.app.lan"), static_cast<std::uint16_t>(RrType::A), 60,
                        {a("10.0.0.2")})
                .has_value());

    const auto outcome = zone.lookup(nm("x.app.lan"), static_cast<std::uint16_t>(RrType::A));
    CHECK(format_ipv4(outcome.answers[0].rdata) == "10.0.0.2");
}

TEST_CASE("exact name without the requested type does not fall through to wildcard") {
    LocalZone zone;
    REQUIRE(zone.upsert(nm("*.app.lan"), static_cast<std::uint16_t>(RrType::A), 60,
                        {a("10.0.0.9")})
                .has_value());
    put_a(zone, "db.app.lan", "10.0.0.10");

    const auto outcome = zone.lookup(nm("db.app.lan"), static_cast<std::uint16_t>(RrType::TXT));
    CHECK(outcome.kind == LookupKind::NoData);
}

TEST_CASE("CNAME is reported as its own step") {
    LocalZone zone;
    auto target = encode_rdata_name(nm("real.lan"));
    REQUIRE(target.has_value());
    REQUIRE(zone.upsert(nm("alias.lan"), static_cast<std::uint16_t>(RrType::CNAME), 60,
                        {target.value()})
                .has_value());

    const auto outcome = zone.lookup(nm("alias.lan"), static_cast<std::uint16_t>(RrType::A));
    CHECK(outcome.kind == LookupKind::Cname);
    REQUIRE(outcome.cname_target.has_value());
    CHECK(outcome.cname_target->to_string() == "real.lan");

    // Querying the CNAME type itself returns the RRset.
    const auto direct =
        zone.lookup(nm("alias.lan"), static_cast<std::uint16_t>(RrType::CNAME));
    CHECK(direct.kind == LookupKind::Answer);
}

TEST_CASE("CNAME cannot coexist with other types") {
    LocalZone zone;
    auto target = encode_rdata_name(nm("real.lan")).value();
    REQUIRE(zone.upsert(nm("alias.lan"), static_cast<std::uint16_t>(RrType::CNAME), 60,
                        {target})
                .has_value());
    CHECK_FALSE(zone.upsert(nm("alias.lan"), static_cast<std::uint16_t>(RrType::A), 60,
                            {a("10.0.0.1")})
                    .has_value());

    LocalZone other;
    put_a(other, "host.lan", "10.0.0.1");
    CHECK_FALSE(other.upsert(nm("host.lan"), static_cast<std::uint16_t>(RrType::CNAME), 60,
                             {target})
                    .has_value());
}

TEST_CASE("validation rejects bad rrsets") {
    LocalZone zone;
    const auto a_type = static_cast<std::uint16_t>(RrType::A);
    CHECK_FALSE(zone.upsert(nm("x.lan"), a_type, 60, {}).has_value());
    CHECK_FALSE(zone.upsert(nm("x.lan"), a_type, 60, {{1, 2, 3}}).has_value());
    CHECK_FALSE(zone.upsert(nm("x.lan"), a_type, kMaxTtl + 1, {a("10.0.0.1")}).has_value());
    CHECK_FALSE(zone.upsert(nm("x.lan"), a_type, 60,
                            std::vector<std::vector<std::uint8_t>>(65, a("10.0.0.1")))
                    .has_value());
    // Types outside the v1 set (e.g. MX) are rejected.
    CHECK_FALSE(zone.upsert(nm("x.lan"), 15, 60, {{0, 1}}).has_value());
    CHECK(zone.empty());
}

TEST_CASE("remove drops empty names") {
    LocalZone zone;
    put_a(zone, "app.lan", "10.0.0.1");
    CHECK(zone.contains(nm("app.lan"), static_cast<std::uint16_t>(RrType::A)));
    CHECK(zone.remove(nm("app.lan"), static_cast<std::uint16_t>(RrType::A)));
    CHECK_FALSE(zone.remove(nm("app.lan"), static_cast<std::uint16_t>(RrType::A)));
    CHECK(zone.empty());
}

TEST_CASE("ANY returns every RRset at the name") {
    LocalZone zone;
    put_a(zone, "app.lan", "10.0.0.1");
    auto txt = encode_rdata_txt("hello");
    REQUIRE(zone.upsert(nm("app.lan"), static_cast<std::uint16_t>(RrType::TXT), 60,
                        {txt.value()})
                .has_value());

    const auto outcome = zone.lookup(nm("app.lan"), static_cast<std::uint16_t>(RrType::ANY));
    CHECK(outcome.kind == LookupKind::Answer);
    CHECK(outcome.answers.size() == 2);
}

TEST_CASE("negative_soa finds the nearest ancestor SOA") {
    LocalZone zone;
    auto soa = encode_rdata_soa(nm("ns.lan"), nm("admin.lan"), 1, 3600, 600, 86400, 60);
    REQUIRE(zone.upsert(nm("lan"), static_cast<std::uint16_t>(RrType::SOA), 60, {soa.value()})
                .has_value());
    put_a(zone, "app.lan", "10.0.0.1");

    const auto found = zone.negative_soa(nm("missing.app.lan"));
    REQUIRE(found.has_value());
    CHECK(found->name.to_string() == "lan");
    CHECK_FALSE(zone.negative_soa(nm("other")).has_value());
}

TEST_CASE("entries and counts") {
    LocalZone zone;
    put_a(zone, "b.lan", "10.0.0.2");
    put_a(zone, "a.lan", "10.0.0.1");
    CHECK(zone.name_count() == 2);
    CHECK(zone.record_count() == 2);

    const auto entries = zone.entries();
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].name.to_string() == "a.lan");
    CHECK(entries[1].name.to_string() == "b.lan");

    zone.clear();
    CHECK(zone.empty());
    CHECK(zone.name_count() == 0);
}
