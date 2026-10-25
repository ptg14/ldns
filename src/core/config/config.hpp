#pragma once

// Configuration model and TOML loading (SPEC §11).
//
// Precedence: CLI flags > LDNS_* environment > config.toml > built-in defaults.
// `Config::load` applies the file and environment; the CLI layer (main.cpp)
// applies flags on top. Unknown keys are rejected so typos in config.toml are
// reported instead of silently ignored.
//
// Environment mapping (LDNS_<SECTION>_<KEY>, upper snake case):
//   network.name -> LDNS_NETWORK_NAME
//   network.sync_port -> LDNS_NETWORK_SYNC_PORT
//   server.listen -> LDNS_SERVER_LISTEN (comma-separated)
//   server.rate_limit_qps -> LDNS_SERVER_RATE_LIMIT_QPS
//   peers.serve_stale -> LDNS_PEERS_SERVE_STALE
//   upstream.servers -> LDNS_UPSTREAM_SERVERS (comma-separated)
//   cache.max_entries -> LDNS_CACHE_MAX_ENTRIES
//   domains.default_ttl -> LDNS_DOMAINS_DEFAULT_TTL
//   ui.language -> LDNS_UI_LANGUAGE

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/util/result.hpp"

namespace ldns::config {

struct NetworkConfig {
    std::string name{"default"};
    std::string key_file;
    std::string interfaces{"auto"};  // "auto" or a list in TOML
    std::string self_interface;
    std::string self_ip;
    std::uint16_t discovery_port{5390};
    std::uint16_t sync_port{5391};
    std::vector<std::string> manual_peers;
};

struct ServerConfig {
    std::vector<std::string> listen{"auto"};
    std::vector<std::string> allow{"private"};
    std::uint32_t rate_limit_qps{200};
};

struct PeersConfig {
    std::uint32_t hedge_ms{250};
    std::uint32_t budget_ms{1500};
    bool serve_stale{true};
    std::uint64_t offline_retention_seconds{24 * 3600};
};

struct UpstreamConfig {
    std::vector<std::string> servers{"auto"};
    std::vector<std::string> fallback{"1.1.1.1", "8.8.8.8"};
    std::uint32_t timeout_ms{2000};
    std::uint32_t retries{1};
};

struct CacheConfig {
    std::uint32_t max_entries{50000};
    std::uint32_t peer_ttl_cap{30};
    std::uint32_t upstream_ttl_cap{300};
    std::uint32_t negative_ttl{5};
};

struct DomainsConfig {
    std::uint32_t default_ttl{60};
    bool warn_public_tld{true};
};

struct SystemConfig {
    bool manage_os_dns{false};
    bool restore_on_exit{true};
};

struct UiConfig {
    std::string language{"auto"};
    std::string theme{"auto"};
};

struct Config {
    NetworkConfig network;
    ServerConfig server;
    PeersConfig peers;
    UpstreamConfig upstream;
    CacheConfig cache;
    DomainsConfig domains;
    SystemConfig system;
    UiConfig ui;

    // Parses TOML text into `out`, replacing the whole configuration.
    static Result<void> parse(std::string_view toml_text, Config& out);

    // Loads a TOML file into `out`. A missing file is not an error (defaults
    // stay in place); a malformed file or wrong types are errors.
    static Result<void> load_file(const std::string& path, Config& out);

    // Parses TOML text. Used by load_file and tests.
    // Applies LDNS_* environment variables on top of the current values.
    void apply_environment();

    // Validates cross-field constraints (ports, upstream listen addresses).
    Result<void> validate() const;
};

// "24h", "90m", "30s" or plain seconds.
Result<std::uint64_t> parse_duration_seconds(std::string_view text);

}  // namespace ldns::config
