#include "doctest.hpp"

#include "core/core_api.hpp"
#include "core/dns/message.hpp"
#include "core/server/query_handler.hpp"

#include <filesystem>
#include <fstream>

using namespace ldns;

namespace {

struct TempDir {
    std::filesystem::path path;

    TempDir() {
        path = std::filesystem::temp_directory_path() /
               ("ldns-core-test-" + std::to_string(::rand()) + "-" +
                std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    std::string file(const std::string& name) const { return (path / name).string(); }
};

CoreApi::Options options_for(const TempDir& dir) {
    CoreApi::Options options;
    options.paths.config_file = dir.file("config.toml");
    options.paths.data_file = dir.file("ldns.db");
    return options;
}

domain::AddRequest add_request(std::string name, std::string type,
                               std::vector<std::string> targets) {
    domain::AddRequest request;
    request.name = std::move(name);
    request.type = std::move(type);
    request.targets = std::move(targets);
    request.force = true;
    return request;
}

std::vector<std::uint8_t> query_packet(std::string_view name, std::uint16_t qtype) {
    dns::Message query;
    query.id = 0x1234;
    query.set_flag(dns::kRd, true);
    query.questions.push_back(
        dns::Question{dns::Name::from_presentation(name).value(), qtype, dns::kClassIn});
    return query.encode().value();
}

}  // namespace

TEST_CASE("CoreApi creates identity, persists records and serves them through the zone") {
    TempDir dir;
    std::string node_hex;
    {
        auto api = CoreApi::create(options_for(dir));
        REQUIRE(api.has_value());

        auto added = api.value()->add_domain(add_request("app.lan", "A", {"192.168.1.10"}));
        REQUIRE(added.has_value());

        auto status = api.value()->status();
        CHECK(status.domain_count == 1);
        CHECK(status.seq == 1);
        CHECK_FALSE(status.short_id.empty());
        node_hex = to_hex(status.node_id);

        // The query handler wired to the same zone answers immediately.
        QueryHandler handler(api.value()->zone());
        auto answer = handler.handle(query_packet("app.lan", 1), QueryContext{});
        REQUIRE(answer.has_value());
        CHECK(answer->message.rcode() == 0);
        REQUIRE(answer->message.answers.size() == 1);
        CHECK(answer->message.answers[0].rdata == std::vector<std::uint8_t>{192, 168, 1, 10});
        CHECK(answer->from_local_zone);
    }

    // Reopen: the record is loaded back and the identity is unchanged.
    {
        auto api = CoreApi::create(options_for(dir));
        REQUIRE(api.has_value());
        auto entries = api.value()->list_domains();
        REQUIRE(entries.has_value());
        REQUIRE(entries.value().size() == 1);
        CHECK(entries.value()[0].name == "app.lan");

        auto status = api.value()->status();
        CHECK(to_hex(status.node_id) == node_hex);
        CHECK(status.seq == 1);
    }
}

TEST_CASE("CoreApi rejects duplicates and removes records from store and zone") {
    TempDir dir;
    auto api = CoreApi::create(options_for(dir));
    REQUIRE(api.has_value());

    REQUIRE(api.value()->add_domain(add_request("app.lan", "A", {"10.0.0.1"})).has_value());
    CHECK(api.value()->add_domain(add_request("app.lan", "A", {"10.0.0.2"})).error().code == 703);

    REQUIRE(api.value()->remove_domain("app.lan", "A").has_value());
    CHECK(api.value()->zone().empty());

    QueryHandler handler(api.value()->zone());
    auto answer = handler.handle(query_packet("app.lan", 1), QueryContext{});
    REQUIRE(answer.has_value());
    CHECK(answer->message.rcode() == static_cast<std::uint8_t>(dns::Rcode::Refused));
}

TEST_CASE("CoreApi import/export through files") {
    TempDir dir;
    auto api = CoreApi::create(options_for(dir));
    REQUIRE(api.has_value());

    const std::string csv = dir.file("zone.csv");
    {
        std::ofstream file(csv);
        file << "name,type,ttl,target\n"
                "one.lan,A,60,10.0.0.1\n"
                "two.lan,A,120,10.0.0.2\n";
    }

    auto report = api.value()->import_file(csv, true);
    REQUIRE(report.has_value());
    CHECK(report.value().imported == 2);
    CHECK(api.value()->status().domain_count == 2);
    CHECK(api.value()->status().seq == 1);  // one seq for the whole import

    auto exported = api.value()->export_zone(false);
    REQUIRE(exported.has_value());
    CHECK(exported.value().find("one.lan,A,60,10.0.0.1") != std::string::npos);

    CHECK_FALSE(api.value()->import_file(dir.file("missing.toml"), true).has_value());
}

TEST_CASE("CoreApi add_self_domain publishes an address and marks the record self") {
    TempDir dir;
    CoreApi::Options options = options_for(dir);
    options.config.network.self_ip = "192.168.1.77";  // pinned: deterministic
    auto api = CoreApi::create(std::move(options));
    REQUIRE(api.has_value());

    auto added = api.value()->add_self_domain("me.lan", "A", 60);
    REQUIRE(added.has_value());
    CHECK(added.value().target_kind == "self");
    REQUIRE(added.value().targets.size() == 1);
    CHECK(added.value().targets[0] == "192.168.1.77");

    auto status = api.value()->status();
    REQUIRE(status.self_addresses.size() == 1);
    CHECK(status.self_addresses[0] == "192.168.1.77");

    // A refresh with unchanged addresses is a no-op (no extra seq bump).
    CHECK(api.value()->refresh_self_addresses().value() == 0);
    CHECK(api.value()->status().seq == 1);
}

TEST_CASE("CoreApi resolves self addresses from the configuration") {
    TempDir dir;
    CoreApi::Options options = options_for(dir);
    options.config.network.self_ip = "2001:db8::42";
    auto api = CoreApi::create(std::move(options));
    REQUIRE(api.has_value());

    auto addresses = api.value()->self_addresses();
    REQUIRE(addresses.has_value());
    REQUIRE(addresses.value().size() == 1);
    CHECK(addresses.value()[0].size() == 16);

    // AAAA self records use the IPv6 address; A records reject it (708).
    auto aaaa = api.value()->add_self_domain("v6.lan", "AAAA", 60);
    REQUIRE(aaaa.has_value());
    CHECK(aaaa.value().targets[0] == "2001:db8::42");

    auto a = api.value()->add_self_domain("v4.lan", "A", 60);
    REQUIRE_FALSE(a.has_value());
    // The pinned address is IPv6-only, so an A record has no usable target.
    CHECK(a.error().code == 708);

    CHECK_FALSE(api.value()->add_self_domain("txt.lan", "TXT", 60).has_value());
}

TEST_CASE("CoreApi reports a clear error for an invalid pinned self_ip") {
    TempDir dir;
    CoreApi::Options options = options_for(dir);
    options.config.network.self_ip = "not-an-ip";
    auto api = CoreApi::create(std::move(options));
    REQUIRE(api.has_value());

    auto addresses = api.value()->self_addresses();
    REQUIRE_FALSE(addresses.has_value());
    CHECK(addresses.error().code == 600);

    auto added = api.value()->add_self_domain("me.lan", "A", 60);
    REQUIRE_FALSE(added.has_value());
    CHECK(added.error().code == 600);
}

TEST_CASE("CoreApi status warns about PSK and reports paths") {
    TempDir dir;
    auto api = CoreApi::create(options_for(dir));
    REQUIRE(api.has_value());

    auto status = api.value()->status();
    CHECK_FALSE(status.psk_configured);
    CHECK_FALSE(status.psk_key_file_found);
    CHECK(status.network_name == "default");
    CHECK(status.data_file == dir.file("ldns.db"));
    CHECK(status.config_file == dir.file("config.toml"));
    CHECK(status.epoch != 0);
}

TEST_CASE("CoreApi reload skips corrupt rows instead of failing to start") {
    TempDir dir;
    {
        auto api = CoreApi::create(options_for(dir));
        REQUIRE(api.has_value());
        REQUIRE(api.value()->add_domain(add_request("good.lan", "A", {"10.0.0.1"})).has_value());
    }

    // Corrupt one row directly in the database.
    {
        auto store = Store::open(StoreOptions{dir.file("ldns.db"), 5000});
        REQUIRE(store.has_value());
        RecordRow row;
        row.name = "bad name";
        row.type = 1;
        row.ttl = 60;
        row.target_kind = "static";
        row.data = {{10, 0, 0, 2}};
        row.created = 1;
        row.updated = 1;
        REQUIRE(store.value().put_record(row).has_value());
    }

    auto api = CoreApi::create(options_for(dir));
    REQUIRE(api.has_value());
    CHECK(api.value()->zone().record_count() == 1);  // only the valid row loaded
    CHECK(api.value()->zone().contains(dns::Name::from_presentation("good.lan").value(), 1));
}

TEST_CASE("CoreApi resolve handles resolution via pipeline") {
    TempDir dir;
    auto api = CoreApi::create(options_for(dir));
    REQUIRE(api.has_value());

    REQUIRE(api.value()->add_domain(add_request("server.lan", "A", {"192.168.1.50"})).has_value());

    auto result = api.value()->resolve("server.lan", "A", true);
    REQUIRE(result.has_value());
    CHECK(result.value().resolved_step == resolver::ResolutionStep::Local);
    CHECK(result.value().response.rcode() == static_cast<std::uint8_t>(dns::Rcode::NoError));
    REQUIRE(result.value().response.answers.size() == 1);
    CHECK(result.value().response.answers[0].rdata[0] == 192);
    CHECK_FALSE(result.value().trace.empty());
}
