#include "doctest.hpp"

#include "core/config/config.hpp"
#include "core/config/paths.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace ldns;

namespace {

class EnvGuard {
public:
    EnvGuard(const char* name, const char* value) : name_(name) {
#if defined(_WIN32)
        char* old = nullptr;
        std::size_t length = 0;
        if (_dupenv_s(&old, &length, name) == 0 && old != nullptr) {
            old_ = old;
            had_ = true;
        }
        std::free(old);
#else
        if (const char* old = std::getenv(name); old != nullptr) {
            old_ = old;
            had_ = true;
        }
#endif
#if defined(_WIN32)
        _putenv_s(name, value == nullptr ? "" : value);
#else
        if (value == nullptr) {
            unsetenv(name);
        } else {
            setenv(name, value, 1);
        }
#endif
    }
    ~EnvGuard() {
#if defined(_WIN32)
        _putenv_s(name_.c_str(), had_ ? old_.c_str() : "");
#else
        if (had_) {
            setenv(name_.c_str(), old_.c_str(), 1);
        } else {
            unsetenv(name_.c_str());
        }
#endif
    }

private:
    std::string name_;
    std::string old_;
    bool had_{false};
};

}  // namespace

TEST_CASE("defaults match SPEC section 11") {
    const config::Config config;
    CHECK(config.network.name == "default");
    CHECK(config.network.discovery_port == 5390);
    CHECK(config.network.sync_port == 5391);
    CHECK(config.server.listen == std::vector<std::string>{"auto"});
    CHECK(config.server.rate_limit_qps == 200);
    CHECK(config.peers.hedge_ms == 250);
    CHECK(config.peers.budget_ms == 1500);
    CHECK(config.peers.serve_stale);
    CHECK(config.peers.offline_retention_seconds == 24 * 3600);
    CHECK(config.upstream.timeout_ms == 2000);
    CHECK(config.upstream.retries == 1);
    CHECK(config.cache.max_entries == 50000);
    CHECK(config.cache.peer_ttl_cap == 30);
    CHECK(config.cache.upstream_ttl_cap == 300);
    CHECK(config.cache.negative_ttl == 5);
    CHECK(config.domains.default_ttl == 60);
    CHECK_FALSE(config.system.manage_os_dns);
    CHECK(config.ui.language == "auto");
}

TEST_CASE("TOML parsing covers every section") {
    const std::string text = R"toml(
[network]
name = "home"
key_file = "/etc/ldns/key"
interfaces = ["eth0", "Wi-Fi"]
self_ip = "192.168.1.10"
discovery_port = 5390
sync_port = 5391
manual_peers = ["192.168.2.5:5391"]

[server]
listen = ["127.0.0.1:53", "192.168.1.10:53"]
allow = ["private", "192.168.5.0/24"]
rate_limit_qps = 500

[peers]
hedge_ms = 100
budget_ms = 900
serve_stale = false
offline_retention = "48h"

[upstream]
servers = ["9.9.9.9"]
fallback = ["1.1.1.1"]
timeout_ms = 1500
retries = 2

[cache]
max_entries = 1000
peer_ttl_cap = 10
upstream_ttl_cap = 120
negative_ttl = 2

[domains]
default_ttl = 30
warn_public_tld = false

[system]
manage_os_dns = true
restore_on_exit = false

[ui]
language = "vi"
theme = "dark"
)toml";

    config::Config parsed;
    auto status = config::Config::parse(text, parsed);
    REQUIRE(status.has_value());

    CHECK(parsed.network.name == "home");
    CHECK(parsed.network.key_file == "/etc/ldns/key");
    CHECK(parsed.network.interfaces == "eth0,Wi-Fi");
    CHECK(parsed.network.self_ip == "192.168.1.10");
    CHECK(parsed.network.manual_peers.size() == 1);
    CHECK(parsed.server.listen.size() == 2);
    CHECK(parsed.server.allow.size() == 2);
    CHECK(parsed.server.rate_limit_qps == 500);
    CHECK(parsed.peers.hedge_ms == 100);
    CHECK(parsed.peers.budget_ms == 900);
    CHECK_FALSE(parsed.peers.serve_stale);
    CHECK(parsed.peers.offline_retention_seconds == 48 * 3600);
    CHECK(parsed.upstream.servers == std::vector<std::string>{"9.9.9.9"});
    CHECK(parsed.upstream.retries == 2);
    CHECK(parsed.cache.max_entries == 1000);
    CHECK(parsed.domains.default_ttl == 30);
    CHECK_FALSE(parsed.domains.warn_public_tld);
    CHECK(parsed.system.manage_os_dns);
    CHECK_FALSE(parsed.system.restore_on_exit);
    CHECK(parsed.ui.language == "vi");
    CHECK(parsed.ui.theme == "dark");
}

TEST_CASE("TOML errors are reported: unknown keys, wrong types, bad durations") {
    config::Config parsed;

    CHECK_FALSE(config::Config::parse("[network]\nname = \"x\"\nnope = 1\n", parsed).has_value());
    CHECK_FALSE(config::Config::parse("[network]\nname = 5\n", parsed).has_value());
    CHECK_FALSE(config::Config::parse("[network]\nsync_port = 70000\n", parsed).has_value());
    CHECK_FALSE(config::Config::parse("[peers]\noffline_retention = \"abc\"\n", parsed).has_value());
    CHECK_FALSE(config::Config::parse("[cache]\nmax_entries = -1\n", parsed).has_value());
    CHECK_FALSE(config::Config::parse("[ui]\nlanguage = \"fr\"\n", parsed).has_value());
    CHECK_FALSE(config::Config::parse("[network\n", parsed).has_value());
    CHECK_FALSE(config::Config::parse("network = 5\n", parsed).has_value());

    // Cross-field constraint: hedge must not exceed the budget.
    CHECK_FALSE(config::Config::parse("[peers]\nhedge_ms = 2000\nbudget_ms = 1000\n", parsed)
                    .has_value());
}

TEST_CASE("parse_duration_seconds accepts s/m/h/d and rejects garbage") {
    CHECK(config::parse_duration_seconds("30").value() == 30);
    CHECK(config::parse_duration_seconds("30s").value() == 30);
    CHECK(config::parse_duration_seconds("5m").value() == 300);
    CHECK(config::parse_duration_seconds("24h").value() == 86400);
    CHECK(config::parse_duration_seconds("2d").value() == 172800);

    CHECK_FALSE(config::parse_duration_seconds("").has_value());
    CHECK_FALSE(config::parse_duration_seconds("h").has_value());
    CHECK_FALSE(config::parse_duration_seconds("10x").has_value());
    CHECK_FALSE(config::parse_duration_seconds("-5").has_value());
    CHECK_FALSE(config::parse_duration_seconds("9999999999999999999999h").has_value());
}

TEST_CASE("load_file treats a missing file as defaults but rejects garbage") {
    const auto missing = std::filesystem::temp_directory_path() / "ldns-no-such-config.toml";
    config::Config config;
    CHECK(config::Config::load_file(missing.string(), config).has_value());
    CHECK(config.network.name == "default");

    const auto path = std::filesystem::temp_directory_path() / "ldns-test-config.toml";
    {
        std::ofstream file(path);
        file << "[network]\nname = \"from-file\"\n";
    }
    CHECK(config::Config::load_file(path.string(), config).has_value());
    CHECK(config.network.name == "from-file");
    std::filesystem::remove(path);
}

TEST_CASE("environment overrides the file") {
    EnvGuard name("LDNS_NETWORK_NAME", "env-net");
    EnvGuard listen("LDNS_SERVER_LISTEN", "127.0.0.1:5300,10.0.0.1:53");
    EnvGuard ttl("LDNS_DOMAINS_DEFAULT_TTL", "42");
    EnvGuard stale("LDNS_PEERS_SERVE_STALE", "false");
    EnvGuard retention("LDNS_PEERS_OFFLINE_RETENTION", "1h");
    EnvGuard invalid("LDNS_CACHE_MAX_ENTRIES", "not-a-number");

    config::Config config;
    config.apply_environment();

    CHECK(config.network.name == "env-net");
    CHECK(config.server.listen.size() == 2);
    CHECK(config.server.listen[1] == "10.0.0.1:53");
    CHECK(config.domains.default_ttl == 42);
    CHECK_FALSE(config.peers.serve_stale);
    CHECK(config.peers.offline_retention_seconds == 3600);
    CHECK(config.cache.max_entries == 50000);  // invalid value ignored
}

TEST_CASE("default_paths resolves per platform and honours LDNS_ overrides") {
    // Service locations come from the install kind, not from privileges.
#if !defined(_WIN32)
    {
        EnvGuard clear_config("LDNS_CONFIG", nullptr);
        EnvGuard clear_data("LDNS_DATA", nullptr);
        CHECK(config::default_paths(config::InstallKind::Service).config_file ==
              "/etc/ldns/config.toml");
        CHECK(config::default_paths(config::InstallKind::Service).data_file ==
              "/var/lib/ldns/ldns.db");
    }
#endif

    EnvGuard config_override("LDNS_CONFIG", "/tmp/ldns-config.toml");
    EnvGuard data_override("LDNS_DATA", "/tmp/ldns-data.db");

    // An explicit override wins for user and service installs alike.
    CHECK(config::default_paths().config_file == "/tmp/ldns-config.toml");
    CHECK(config::default_paths().data_file == "/tmp/ldns-data.db");
    CHECK(config::default_paths(config::InstallKind::Service).config_file ==
          "/tmp/ldns-config.toml");
}

TEST_CASE("validate rejects empty network name and zero rate limit") {
    config::Config config;
    CHECK(config.validate().has_value());

    config.network.name.clear();
    CHECK_FALSE(config.validate().has_value());

    config::Config other;
    other.server.rate_limit_qps = 0;
    CHECK_FALSE(other.validate().has_value());
}
