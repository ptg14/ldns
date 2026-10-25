#include "doctest.hpp"

#include "core/dns/local_zone.hpp"
#include "core/domain/domain_service.hpp"
#include "core/domain/import_export.hpp"
#include "core/domain/local_zone_sink.hpp"
#include "core/store/store.hpp"

#include <algorithm>

using namespace ldns;
using namespace ldns::domain;

namespace {

struct Fixture {
    Store store;
    dns::LocalZone zone;
    LocalZoneSink sink{zone};
    HlcClock clock;
    DomainService service{store, sink, clock};
    std::vector<DomainEvent> events;

    Fixture() : store(Store::open(StoreOptions{":memory:", 5000}).value()) {
        (void)store.identity();
        service.set_event_handler([this](const DomainEvent& event) { events.push_back(event); });
    }
};

AddRequest request(std::string name, std::string type, std::vector<std::string> targets,
                   std::uint32_t ttl = 60, bool force = true) {
    AddRequest out;
    out.name = std::move(name);
    out.type = std::move(type);
    out.targets = std::move(targets);
    out.ttl = ttl;
    out.force = force;
    return out;
}

std::vector<std::uint8_t> ipv4(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d) {
    return {a, b, c, d};
}

std::string toml_control_escape() {
    // The TOML output for U+0001 is backslash + "u0001".
    std::string out;
    out.push_back(char(92));
    out += "u0001";
    return out;
}

}  // namespace

TEST_CASE("add writes the record, mirrors it into the zone and bumps seq once") {
    Fixture fx;
    auto added = fx.service.add(request("app.lan", "A", {"192.168.1.10"}));
    REQUIRE(added.has_value());
    CHECK(added.value().name == "app.lan");
    CHECK(added.value().type == "A");
    CHECK(added.value().target_kind == "static");
    REQUIRE(added.value().targets.size() == 1);
    CHECK(added.value().targets[0] == "192.168.1.10");

    // Zone answers immediately.
    const auto name = dns::Name::from_presentation("app.lan").value();
    const auto outcome = fx.zone.lookup(name, 1);
    CHECK(outcome.kind == dns::LookupKind::Answer);
    REQUIRE(outcome.answers.size() == 1);
    CHECK(outcome.answers[0].rdata == ipv4(192, 168, 1, 10));

    // Identity seq advanced exactly once; one event emitted.
    CHECK(fx.store.identity().value().seq == 1);
    REQUIRE(fx.events.size() == 1);
    CHECK(fx.events[0].kind == EventType::RecordPut);
    CHECK(fx.events[0].seq == 1);
    CHECK(fx.events[0].name == "app.lan");
}

TEST_CASE("add normalizes names and rejects duplicates with code 703") {
    Fixture fx;
    REQUIRE(fx.service.add(request("App.LAN.", "A", {"192.168.1.10"})).has_value());

    const auto rows = fx.service.list();
    REQUIRE(rows.has_value());
    REQUIRE(rows.value().size() == 1);
    CHECK(rows.value()[0].name == "app.lan");

    const auto duplicate = fx.service.add(request("app.lan", "A", {"192.168.1.11"}));
    REQUIRE_FALSE(duplicate.has_value());
    CHECK(duplicate.error().code == 703);
}

TEST_CASE("set replaces values and preserves created_at") {
    Fixture fx;
    const auto added = fx.service.add(request("app.lan", "A", {"192.168.1.10"}));
    REQUIRE(added.has_value());

    auto updated = fx.service.set(request("app.lan", "A", {"192.168.1.20", "192.168.1.21"}));
    REQUIRE(updated.has_value());
    CHECK(updated.value().targets.size() == 2);
    CHECK(updated.value().created == added.value().created);
    CHECK(updated.value().updated >= added.value().updated);

    const auto outcome =
        fx.zone.lookup(dns::Name::from_presentation("app.lan").value(), 1);
    REQUIRE(outcome.answers.size() == 2);
    CHECK(fx.store.identity().value().seq == 2);
}

TEST_CASE("set on a missing record creates it (upsert)") {
    Fixture fx;
    auto updated = fx.service.set(request("new.lan", "TXT", {"hello"}));
    REQUIRE(updated.has_value());
    CHECK(updated.value().created == updated.value().updated);
    CHECK(fx.store.identity().value().seq == 1);
}

TEST_CASE("remove deletes from store and zone and reports 704 when missing") {
    Fixture fx;
    REQUIRE(fx.service.add(request("app.lan", "A", {"192.168.1.10"})).has_value());
    REQUIRE(fx.service.remove("app.lan", "A").has_value());

    CHECK(fx.service.list().value().empty());
    CHECK(fx.zone.empty());
    CHECK(fx.store.identity().value().seq == 2);
    REQUIRE(fx.events.size() == 2);
    CHECK(fx.events[1].kind == EventType::RecordDel);

    const auto missing = fx.service.remove("app.lan", "A");
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().code == 704);
}

TEST_CASE("CNAME cannot coexist with other records (code 706)") {
    Fixture fx;
    REQUIRE(fx.service.add(request("alias.lan", "A", {"192.168.1.10"})).has_value());

    const auto cname = fx.service.add(request("alias.lan", "CNAME", {"other.lan"}));
    REQUIRE_FALSE(cname.has_value());
    CHECK(cname.error().code == 706);

    Fixture fx2;
    REQUIRE(fx2.service.add(request("alias.lan", "CNAME", {"other.lan"})).has_value());
    const auto a = fx2.service.add(request("alias.lan", "A", {"192.168.1.10"}));
    REQUIRE_FALSE(a.has_value());
    CHECK(a.error().code == 706);
}

TEST_CASE("invalid names, types, values and TTLs are rejected") {
    Fixture fx;

    CHECK(fx.service.add(request("bad..name", "A", {"192.168.1.10"})).error().code == 700);
    CHECK(fx.service.add(request("", "A", {"192.168.1.10"})).error().code == 700);
    CHECK(fx.service.add(request("app.lan", "MX", {"mail.lan"})).error().code == 701);
    CHECK(fx.service.add(request("app.lan", "SOA", {"x"})).error().code == 701);
    CHECK(fx.service.add(request("app.lan", "A", {"999.1.1.1"})).error().code == 702);
    CHECK(fx.service.add(request("app.lan", "AAAA", {"192.168.1.10"})).error().code == 702);
    CHECK(fx.service.add(request("app.lan", "CNAME", {"a", "b"})).error().code == 702);
    CHECK(fx.service.add(request("app.lan", "A", {}, 60)).error().code == 708);
    CHECK(fx.service.add(request("app.lan", "A", {"192.168.1.10"}, 2'147'483'648u))
              .error()
              .code == 702);
}

TEST_CASE("public TLD confirmation is required unless force is set") {
    Fixture fx;

    auto validation = fx.service.validate_name("google.com");
    REQUIRE(validation.has_value());
    CHECK(validation.value().needs_tld_confirmation);

    const auto blocked = fx.service.add(request("google.com", "A", {"192.168.1.10"}, 60, false));
    REQUIRE_FALSE(blocked.has_value());
    CHECK(blocked.error().code == 705);
    CHECK(fx.store.identity().value().seq == 0);  // nothing written

    CHECK(fx.service.add(request("google.com", "A", {"192.168.1.10"}, 60, true)).has_value());

    const auto lan = fx.service.validate_name("app.lan");
    REQUIRE(lan.has_value());
    CHECK_FALSE(lan.value().needs_tld_confirmation);

    const auto local = fx.service.validate_name("printer.local");
    REQUIRE(local.has_value());
    CHECK(local.value().mdns_conflict);
    CHECK_FALSE(local.value().warning.empty());
}

TEST_CASE("multiple values form an RRset; duplicates collapse") {
    Fixture fx;
    auto added = fx.service.add(request("multi.lan", "A", {"10.0.0.2", "10.0.0.1", "10.0.0.2"}));
    REQUIRE(added.has_value());
    CHECK(added.value().targets.size() == 2);
    // Canonical order (protocol.md §2.4).
    CHECK(added.value().targets[0] == "10.0.0.1");
    CHECK(added.value().targets[1] == "10.0.0.2");
}

TEST_CASE("list filters by type") {
    Fixture fx;
    REQUIRE(fx.service.add(request("a.lan", "A", {"10.0.0.1"})).has_value());
    REQUIRE(fx.service.add(request("b.lan", "TXT", {"hello"})).has_value());
    REQUIRE(fx.service.add(request("c.lan", "AAAA", {"2001:db8::1"})).has_value());

    CHECK(fx.service.list().value().size() == 3);
    CHECK(fx.service.list(std::string("TXT")).value().size() == 1);
    CHECK(fx.service.list(std::string("a")).value()[0].type == "A");  // case-insensitive
    CHECK_FALSE(fx.service.list(std::string("MX")).has_value());
}

TEST_CASE("wildcard and TXT records round-trip") {
    Fixture fx;
    REQUIRE(fx.service.add(request("*.wild.lan", "A", {"10.0.0.1"})).has_value());
    REQUIRE(fx.service.add(request("text.lan", "TXT", {"hello world"})).has_value());

    const auto lookup = fx.zone.lookup(dns::Name::from_presentation("deep.child.wild.lan").value(), 1);
    CHECK(lookup.kind == dns::LookupKind::Answer);

    const auto txt = fx.zone.lookup(dns::Name::from_presentation("text.lan").value(), 16);
    REQUIRE(txt.answers.size() == 1);
    CHECK(dns::decode_rdata_txt(txt.answers[0].rdata).value() == "hello world");
}

TEST_CASE("refresh_self_records rewrites only rows marked self") {
    Fixture fx;

    AddRequest self_record = request("me.lan", "A", {"192.168.1.10"});
    self_record.kind = TargetKind::Self;
    REQUIRE(fx.service.add(self_record).has_value());
    REQUIRE(fx.service.add(request("static.lan", "A", {"10.9.9.9"})).has_value());

    const std::vector<SelfAddressBytes> addresses = {
        {192, 168, 1, 20},
        {10, 1, 2, 3},
    };
    const auto changed = fx.service.refresh_self_records(addresses);
    REQUIRE(changed.has_value());
    CHECK(changed.value() == 1);

    const auto entries = fx.service.list();
    REQUIRE(entries.has_value());
    for (const auto& entry : entries.value()) {
        if (entry.name == "me.lan") {
            REQUIRE(entry.targets.size() == 2);
            CHECK(entry.targets[0] == "10.1.2.3");
            CHECK(entry.targets[1] == "192.168.1.20");
            CHECK(entry.target_kind == "self");
        } else {
            CHECK(entry.targets[0] == "10.9.9.9");
            CHECK(entry.target_kind == "static");
        }
    }

    // Nothing changed the second time: no seq bump.
    const auto seq_before = fx.store.identity().value().seq;
    CHECK(fx.service.refresh_self_records(addresses).value() == 0);
    CHECK(fx.store.identity().value().seq == seq_before);
}

TEST_CASE("add_many is atomic and uses one seq for the whole batch") {
    Fixture fx;
    std::vector<AddRequest> batch;
    for (int i = 0; i < 100; ++i) {
        batch.push_back(request("host" + std::to_string(i) + ".lan", "A",
                                {"10.0.0." + std::to_string(i % 250 + 1)}));
    }
    auto bulk = fx.service.add_many(batch, true);
    REQUIRE(bulk.has_value());
    CHECK(bulk.value().applied == 100);
    CHECK(bulk.value().seq == 1);
    CHECK(fx.service.list().value().size() == 100);

    // A bad row anywhere rejects the whole batch.
    std::vector<AddRequest> with_bad = batch;
    with_bad.push_back(request("broken.lan", "A", {"not-an-ip"}));
    const auto seq_before = fx.store.identity().value().seq;
    CHECK_FALSE(fx.service.add_many(with_bad, true).has_value());
    CHECK(fx.store.identity().value().seq == seq_before);
    CHECK(fx.service.list().value().size() == 100);
}

TEST_CASE("add_many deduplicates (name,type) keeping the last row") {
    Fixture fx;
    std::vector<AddRequest> batch = {
        request("dup.lan", "A", {"10.0.0.1"}),
        request("dup.lan", "A", {"10.0.0.2"}),
    };
    auto bulk = fx.service.add_many(batch, true);
    REQUIRE(bulk.has_value());
    CHECK(bulk.value().applied == 1);
    const auto entries = fx.service.list().value();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].targets[0] == "10.0.0.2");
}

TEST_CASE("import/export TOML and CSV round-trip") {
    Fixture source;
    REQUIRE(source.service.add(request("app.lan", "A", {"192.168.1.10"})).has_value());
    REQUIRE(source.service.add(request("mail.lan", "AAAA", {"2001:db8::5"})).has_value());
    REQUIRE(source.service.add(request("txt.lan", "TXT", {"hello"})).has_value());

    const auto toml_text = export_domains(source.service.list().value(), true);
    CHECK(toml_text.find("[[record]]") != std::string::npos);
    CHECK(toml_text.find("app.lan") != std::string::npos);

    Fixture target;
    auto report = import_domains(target.service, toml_text, true, true);
    REQUIRE(report.has_value());
    CHECK(report.value().imported == 3);
    CHECK(target.service.list().value().size() == 3);

    const auto csv_text = export_domains(source.service.list().value(), false);
    CHECK(csv_text.starts_with("name,type,ttl,target"));

    Fixture from_csv;
    auto csv_report = import_domains(from_csv.service, csv_text, false, true);
    REQUIRE(csv_report.has_value());
    CHECK(csv_report.value().imported == 3);
    CHECK(from_csv.service.list().value().size() == 3);
}

TEST_CASE("CSV import tolerates comments, blank lines and a header") {
    Fixture fx;
    const std::string text =
        "# comment\n"
        "name,type,ttl,target\n"
        "\n"
        "a.lan,A,60,10.0.0.1\n"
        "b.lan,TXT,,hello\n"
        "c.lan,A,60,10.0.0.5;10.0.0.6\n";
    auto report = import_domains(fx.service, text, false, true);
    REQUIRE(report.has_value());
    CHECK(report.value().imported == 3);

    const auto entries = fx.service.list().value();
    REQUIRE(entries.size() == 3);
    for (const auto& entry : entries) {
        if (entry.name == "b.lan") CHECK(entry.ttl == 60);  // default kept
        if (entry.name == "c.lan") CHECK(entry.targets.size() == 2);
    }
}

TEST_CASE("import errors leave the zone untouched") {
    Fixture fx;
    REQUIRE(fx.service.add(request("keep.lan", "A", {"10.0.0.1"})).has_value());
    const auto seq_before = fx.store.identity().value().seq;

    const std::string bad = "name,type,ttl,target\nnew.lan,A,60,10.0.0.2\nbad.lan,A,60,not-ip\n";
    CHECK_FALSE(import_domains(fx.service, bad, false, true).has_value());
    CHECK(fx.service.list().value().size() == 1);
    CHECK(fx.store.identity().value().seq == seq_before);

    CHECK_FALSE(import_domains(fx.service, "[[record]]\nname = \"x\"\n", true, true).has_value());
    CHECK_FALSE(import_domains(fx.service, "not toml [", true, true).has_value());
}

TEST_CASE("import without force rejects public TLDs") {
    Fixture fx;
    const std::string text = "name,type,ttl,target\ngoogle.com,A,60,10.0.0.1\n";
    const auto report = import_domains(fx.service, text, false, false);
    REQUIRE_FALSE(report.has_value());
    CHECK(report.error().code == 705);
}

TEST_CASE("import rejects a CNAME and other type for the same name in one batch") {
    Fixture fx;
    const std::string text =
        "name,type,ttl,target\n"
        "app.lan,A,60,10.0.0.1\n"
        "app.lan,CNAME,60,other.lan\n";
    const auto report = import_domains(fx.service, text, false, true);
    REQUIRE_FALSE(report.has_value());
    CHECK(report.error().code == 706);
    CHECK(fx.service.list().value().empty());
    CHECK(fx.zone.empty());
    CHECK(fx.store.identity().value().seq == 0);
}

TEST_CASE("import rejects self targets on non-address types") {
    Fixture fx;
    const std::string toml = R"toml(
[[record]]
name = "txt.lan"
type = "TXT"
target_kind = "self"
target = ["hello"]
)toml";
    const auto report = import_domains(fx.service, toml, true, true);
    REQUIRE_FALSE(report.has_value());
    CHECK(report.error().code == 702);
}

TEST_CASE("CSV round-trips TXT values with commas and quotes") {
    Fixture fx;
    REQUIRE(fx.service.add(request("comma.lan", "TXT", {"hello, world"})).has_value());
    REQUIRE(fx.service.add(request("quote.lan", "TXT", {R"(say "hi")"})).has_value());

    const auto exported = export_domains(fx.service.list().value(), false);
    CHECK(exported.find("\"hello, world\"") != std::string::npos);

    Fixture target;
    const auto report = import_domains(target.service, exported, false, true);
    REQUIRE(report.has_value());
    const auto entries = target.service.list().value();
    REQUIRE(entries.size() == 2);
    for (const auto& entry : entries) {
        if (entry.name == "comma.lan") CHECK(entry.targets[0] == "hello, world");
        if (entry.name == "quote.lan") CHECK(entry.targets[0] == "say \"hi\"");
    }
}

TEST_CASE("TOML export escapes control characters and re-imports") {
    Fixture fx;
    const std::string control = std::string("a") + char(1) + "b";
    REQUIRE(fx.service.add(request("ctrl.lan", "TXT", {control})).has_value());

    const auto exported = export_domains(fx.service.list().value(), true);
    CHECK(exported.find(toml_control_escape()) != std::string::npos);

    Fixture target;
    REQUIRE(import_domains(target.service, exported, true, true).has_value());
    const auto entries = target.service.list().value();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].targets[0] == control);
}

TEST_CASE("warn_public_tld=false allows public TLDs without force") {
    Fixture fx;
    fx.service.set_warn_public_tld(false);
    CHECK(fx.service.add(request("example.com", "A", {"10.0.0.1"}, 60, false)).has_value());

    Fixture strict;
    const auto blocked = strict.service.add(request("example.com", "A", {"10.0.0.1"}, 60, false));
    REQUIRE_FALSE(blocked.has_value());
    CHECK(blocked.error().code == 705);
}
