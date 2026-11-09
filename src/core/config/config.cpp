#include "core/config/config.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
#include <unordered_set>

#include <toml++/toml.hpp>

#include "core/util/logger.hpp"

namespace ldns::config {
namespace {

namespace fs = std::filesystem;

std::string lower_ascii(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

std::vector<std::string> split_list(std::string_view text) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t comma = text.find(',', start);
        const std::size_t end = comma == std::string_view::npos ? text.size() : comma;
        std::string_view item = text.substr(start, end - start);
        while (!item.empty() && std::isspace(static_cast<unsigned char>(item.front()))) {
            item.remove_prefix(1);
        }
        while (!item.empty() && std::isspace(static_cast<unsigned char>(item.back()))) {
            item.remove_suffix(1);
        }
        if (!item.empty()) out.emplace_back(item);
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    return out;
}

std::string getenv_string(const char* name) {
#if defined(_WIN32)
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) return {};
    std::string out(value);
    std::free(value);
    return out;
#else
    if (const char* value = std::getenv(name); value != nullptr) return value;
    return {};
#endif
}

// Reads a string-or-array-of-strings value. A scalar becomes a single element.
Result<std::vector<std::string>> read_string_list(const toml::node_view<const toml::node>& node,
                                                  std::string_view key) {
    std::vector<std::string> out;
    if (!node) return out;
    if (const auto* array = node.as_array()) {
        for (const auto& element : *array) {
            const auto value = element.value<std::string>();
            if (!value) return Error{500, std::string(key) + " must be a string or array of strings"};
            out.push_back(*value);
        }
        return out;
    }
    if (const auto value = node.value<std::string>()) {
        out.push_back(*value);
        return out;
    }
    return Error{500, std::string(key) + " must be a string or array of strings"};
}

Result<void> read_string(const toml::node_view<const toml::node>& node, std::string_view key,
                         std::string& out) {
    if (!node) return {};
    const auto value = node.value<std::string>();
    if (!value) return Error{501, std::string(key) + " must be a string"};
    out = *value;
    return {};
}

Result<void> read_bool(const toml::node_view<const toml::node>& node, std::string_view key,
                       bool& out) {
    if (!node) return {};
    const auto value = node.value<bool>();
    if (!value) return Error{502, std::string(key) + " must be a boolean"};
    out = *value;
    return {};
}

Result<void> read_u32(const toml::node_view<const toml::node>& node, std::string_view key,
                      std::uint32_t& out, std::uint32_t min_value = 0,
                      std::uint32_t max_value = 0xFFFFFFFFu) {
    if (!node) return {};
    const auto value = node.value<std::int64_t>();
    if (!value || *value < 0) return Error{503, std::string(key) + " must be a non-negative integer"};
    const auto signed_value = *value;
    if (static_cast<std::uint64_t>(signed_value) < min_value ||
        static_cast<std::uint64_t>(signed_value) > max_value) {
        return Error{503, std::string(key) + " is out of range"};
    }
    out = static_cast<std::uint32_t>(signed_value);
    return {};
}

Result<void> read_u16(const toml::node_view<const toml::node>& node, std::string_view key,
                      std::uint16_t& out, std::uint16_t min_value) {
    std::uint32_t value = out;
    if (auto status = read_u32(node, key, value, min_value, 65535); !status) return status;
    out = static_cast<std::uint16_t>(value);
    return {};
}

// Rejects unknown keys inside a table so typos in config.toml surface.
Result<void> check_keys(const toml::table& table, std::string_view section,
                        const std::unordered_set<std::string>& allowed) {
    for (const auto& [key, node] : table) {
        (void)node;
        const std::string name(key.str());
        if (allowed.count(name) == 0) {
            return Error{504, "unknown key [" + std::string(section) + "]." + name};
        }
    }
    return {};
}

Result<const toml::table*> read_section(const toml::table& root, std::string_view name,
                                        const std::unordered_set<std::string>& allowed) {
    const auto node = root[name];
    if (!node) return static_cast<const toml::table*>(nullptr);
    const auto* table = node.as_table();
    if (table == nullptr) return Error{505, "[" + std::string(name) + "] must be a table"};
    if (auto status = check_keys(*table, name, allowed); !status) return status.error();
    return table;
}

}  // namespace

Result<std::uint64_t> parse_duration_seconds(std::string_view text) {
    if (text.empty()) return Error{506, "empty duration"};
    std::size_t digits = 0;
    std::uint64_t value = 0;
    while (digits < text.size() && std::isdigit(static_cast<unsigned char>(text[digits]))) {
        const std::uint64_t digit = static_cast<std::uint64_t>(text[digits] - '0');
        if (value > (UINT64_MAX - digit) / 10) return Error{506, "duration overflows"};
        value = value * 10 + digit;
        ++digits;
    }
    if (digits == 0) return Error{506, "invalid duration"};
    const std::string_view suffix = text.substr(digits);
    std::uint64_t multiplier = 1;
    if (suffix.empty() || suffix == "s") {
        multiplier = 1;
    } else if (suffix == "m") {
        multiplier = 60;
    } else if (suffix == "h") {
        multiplier = 3600;
    } else if (suffix == "d") {
        multiplier = 86400;
    } else {
        return Error{506, "invalid duration unit (use s, m, h or d)"};
    }
    if (value > UINT64_MAX / multiplier) return Error{506, "duration overflows"};
    return value * multiplier;
}

Result<void> Config::parse(std::string_view toml_text, Config& out) {
    toml::table table;
    try {
        table = toml::parse(toml_text);
    } catch (const toml::parse_error& error) {
        std::ostringstream message;
        message << error;
        return Error{507, "config.toml: " + message.str()};
    }
    const toml::table& root = table;

    const toml::table* section = nullptr;
    const auto load_section = [&](std::string_view name,
                                  std::initializer_list<const char*> allowed) -> Result<void> {
        auto result =
            read_section(root, name, std::unordered_set<std::string>(allowed.begin(), allowed.end()));
        if (!result) return result.error();
        section = result.value();
        return {};
    };

    // --- [network] ---------------------------------------------------------
    if (auto status = load_section("network",
                                   {"name", "key_file", "interfaces", "self_interface", "self_ip",
                                    "discovery_port", "sync_port", "manual_peers"});
        !status) {
        return status;
    }
    if (section != nullptr) {
        const auto& tbl = *section;
        if (auto status = read_string(tbl["name"], "network.name", out.network.name); !status) {
            return status;
        }
        if (auto status = read_string(tbl["key_file"], "network.key_file", out.network.key_file);
            !status) {
            return status;
        }
        if (const auto interfaces = tbl["interfaces"]; interfaces) {
            if (const auto single = interfaces.value<std::string>()) {
                out.network.interfaces = *single;
            } else {
                auto list = read_string_list(interfaces, "network.interfaces");
                if (!list) return list.error();
                out.network.interfaces.clear();
                for (const auto& item : list.value()) {
                    if (!out.network.interfaces.empty()) out.network.interfaces.push_back(',');
                    out.network.interfaces += item;
                }
            }
        }
        if (auto status =
                read_string(tbl["self_interface"], "network.self_interface", out.network.self_interface);
            !status) {
            return status;
        }
        if (auto status = read_string(tbl["self_ip"], "network.self_ip", out.network.self_ip);
            !status) {
            return status;
        }
        if (auto status = read_u16(tbl["discovery_port"], "network.discovery_port",
                                   out.network.discovery_port, 1);
            !status) {
            return status;
        }
        if (auto status =
                read_u16(tbl["sync_port"], "network.sync_port", out.network.sync_port, 1);
            !status) {
            return status;
        }
        if (auto status =
                [&]() -> Result<void> {
            auto list = read_string_list(tbl["manual_peers"], "network.manual_peers");
            if (!list) return list.error();
            if (tbl["manual_peers"]) out.network.manual_peers = std::move(list.value());
            return {};
        }();
            !status) {
            return status;
        }
    }

    // --- [server] ----------------------------------------------------------
    if (auto status = load_section("server", {"listen", "allow", "rate_limit_qps"}); !status) {
        return status;
    }
    if (section != nullptr) {
        const auto& tbl = *section;
        if (auto list = read_string_list(tbl["listen"], "server.listen"); list) {
            if (tbl["listen"]) out.server.listen = std::move(list.value());
        } else {
            return list.error();
        }
        if (auto list = read_string_list(tbl["allow"], "server.allow"); list) {
            if (tbl["allow"]) out.server.allow = std::move(list.value());
        } else {
            return list.error();
        }
        if (auto status = read_u32(tbl["rate_limit_qps"], "server.rate_limit_qps",
                                   out.server.rate_limit_qps, 1);
            !status) {
            return status;
        }
    }

    // --- [peers] -----------------------------------------------------------
    if (auto status =
            load_section("peers", {"hedge_ms", "budget_ms", "serve_stale", "offline_retention"});
        !status) {
        return status;
    }
    if (section != nullptr) {
        const auto& tbl = *section;
        if (auto status = read_u32(tbl["hedge_ms"], "peers.hedge_ms", out.peers.hedge_ms, 1);
            !status) {
            return status;
        }
        if (auto status = read_u32(tbl["budget_ms"], "peers.budget_ms", out.peers.budget_ms, 1);
            !status) {
            return status;
        }
        if (auto status = read_bool(tbl["serve_stale"], "peers.serve_stale", out.peers.serve_stale);
            !status) {
            return status;
        }
        if (const auto retention = tbl["offline_retention"]; retention) {
            const auto text = retention.value<std::string>();
            if (!text) return Error{508, "peers.offline_retention must be a string such as \"24h\""};
            auto seconds = parse_duration_seconds(*text);
            if (!seconds) return seconds.error();
            out.peers.offline_retention_seconds = seconds.value();
        }
    }

    // --- [upstream] --------------------------------------------------------
    if (auto status = load_section("upstream", {"servers", "fallback", "timeout_ms", "retries"});
        !status) {
        return status;
    }
    if (section != nullptr) {
        const auto& tbl = *section;
        if (auto list = read_string_list(tbl["servers"], "upstream.servers"); list) {
            if (tbl["servers"]) out.upstream.servers = std::move(list.value());
        } else {
            return list.error();
        }
        if (auto list = read_string_list(tbl["fallback"], "upstream.fallback"); list) {
            if (tbl["fallback"]) out.upstream.fallback = std::move(list.value());
        } else {
            return list.error();
        }
        if (auto status =
                read_u32(tbl["timeout_ms"], "upstream.timeout_ms", out.upstream.timeout_ms, 1);
            !status) {
            return status;
        }
        if (auto status = read_u32(tbl["retries"], "upstream.retries", out.upstream.retries, 0);
            !status) {
            return status;
        }
    }

    // --- [cache] -----------------------------------------------------------
    if (auto status = load_section("cache", {"max_entries", "peer_ttl_cap", "upstream_ttl_cap",
                                             "negative_ttl"});
        !status) {
        return status;
    }
    if (section != nullptr) {
        const auto& tbl = *section;
        if (auto status =
                read_u32(tbl["max_entries"], "cache.max_entries", out.cache.max_entries, 1);
            !status) {
            return status;
        }
        if (auto status = read_u32(tbl["peer_ttl_cap"], "cache.peer_ttl_cap",
                                   out.cache.peer_ttl_cap, 1);
            !status) {
            return status;
        }
        if (auto status = read_u32(tbl["upstream_ttl_cap"], "cache.upstream_ttl_cap",
                                   out.cache.upstream_ttl_cap, 1);
            !status) {
            return status;
        }
        if (auto status =
                read_u32(tbl["negative_ttl"], "cache.negative_ttl", out.cache.negative_ttl, 0);
            !status) {
            return status;
        }
    }

    // --- [domains] ---------------------------------------------------------
    if (auto status = load_section("domains", {"default_ttl", "warn_public_tld"}); !status) {
        return status;
    }
    if (section != nullptr) {
        const auto& tbl = *section;
        if (auto status = read_u32(tbl["default_ttl"], "domains.default_ttl",
                                   out.domains.default_ttl, 1, 2'147'483'647);
            !status) {
            return status;
        }
        if (auto status =
                read_bool(tbl["warn_public_tld"], "domains.warn_public_tld", out.domains.warn_public_tld);
            !status) {
            return status;
        }
    }

    // --- [system] ----------------------------------------------------------
    if (auto status = load_section("system", {"manage_os_dns", "restore_on_exit"}); !status) {
        return status;
    }
    if (section != nullptr) {
        const auto& tbl = *section;
        if (auto status =
                read_bool(tbl["manage_os_dns"], "system.manage_os_dns", out.system.manage_os_dns);
            !status) {
            return status;
        }
        if (auto status = read_bool(tbl["restore_on_exit"], "system.restore_on_exit",
                                    out.system.restore_on_exit);
            !status) {
            return status;
        }
    }

    // --- [ui] --------------------------------------------------------------
    if (auto status = load_section("ui", {"language", "theme"}); !status) {
        return status;
    }
    if (section != nullptr) {
        const auto& tbl = *section;
        if (auto status = read_string(tbl["language"], "ui.language", out.ui.language); !status) {
            return status;
        }
        if (auto status = read_string(tbl["theme"], "ui.theme", out.ui.theme); !status) {
            return status;
        }
    }

    return out.validate();
}

Result<void> Config::load_file(const std::string& path, Config& out) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return {};
    if (ec) return Error{509, "config: " + path + ": " + ec.message()};
    if (!fs::is_regular_file(path, ec)) return Error{509, "config: " + path + " is not a file"};

    std::ifstream file(path, std::ios::binary);
    if (!file) return Error{509, "config: cannot open " + path};
    std::ostringstream contents;
    contents << file.rdbuf();
    return parse(contents.str(), out);
}

void Config::apply_environment() {
    auto set_string = [](const char* env, std::string& target) {
        const std::string value = getenv_string(env);
        if (!value.empty()) target = value;
    };
    auto set_list = [](const char* env, std::vector<std::string>& target) {
        const std::string value = getenv_string(env);
        if (!value.empty()) target = split_list(value);
    };
    auto set_u32 = [](const char* env, std::uint32_t& target) {
        const std::string value = getenv_string(env);
        if (value.empty()) return;
        try {
            const auto parsed = std::stoull(value);
            if (parsed > 0 && parsed <= 0xFFFFFFFFull) target = static_cast<std::uint32_t>(parsed);
        } catch (const std::exception&) {
            // Invalid environment values are ignored in favour of the file.
        }
    };
    auto set_u16 = [](const char* env, std::uint16_t& target) {
        const std::string value = getenv_string(env);
        if (value.empty()) return;
        try {
            const auto parsed = std::stoull(value);
            if (parsed > 0 && parsed <= 65535) target = static_cast<std::uint16_t>(parsed);
        } catch (const std::exception&) {
        }
    };
    auto set_bool = [](const char* env, bool& target) {
        std::string value = getenv_string(env);
        if (value.empty()) return;
        value = lower_ascii(value);
        if (value == "1" || value == "true" || value == "yes" || value == "on") target = true;
        if (value == "0" || value == "false" || value == "no" || value == "off") target = false;
    };

    set_string("LDNS_NETWORK_NAME", network.name);
    set_string("LDNS_NETWORK_KEY_FILE", network.key_file);
    set_string("LDNS_NETWORK_SELF_INTERFACE", network.self_interface);
    set_string("LDNS_NETWORK_SELF_IP", network.self_ip);
    set_u16("LDNS_NETWORK_DISCOVERY_PORT", network.discovery_port);
    set_u16("LDNS_NETWORK_SYNC_PORT", network.sync_port);
    set_list("LDNS_NETWORK_MANUAL_PEERS", network.manual_peers);

    set_list("LDNS_SERVER_LISTEN", server.listen);
    set_list("LDNS_SERVER_ALLOW", server.allow);
    set_u32("LDNS_SERVER_RATE_LIMIT_QPS", server.rate_limit_qps);

    set_u32("LDNS_PEERS_HEDGE_MS", peers.hedge_ms);
    set_u32("LDNS_PEERS_BUDGET_MS", peers.budget_ms);
    set_bool("LDNS_PEERS_SERVE_STALE", peers.serve_stale);
    if (const std::string value = getenv_string("LDNS_PEERS_OFFLINE_RETENTION"); !value.empty()) {
        if (auto seconds = parse_duration_seconds(value); seconds) {
            peers.offline_retention_seconds = seconds.value();
        }
    }

    set_list("LDNS_UPSTREAM_SERVERS", upstream.servers);
    set_list("LDNS_UPSTREAM_FALLBACK", upstream.fallback);
    set_u32("LDNS_UPSTREAM_TIMEOUT_MS", upstream.timeout_ms);
    set_u32("LDNS_UPSTREAM_RETRIES", upstream.retries);

    set_u32("LDNS_CACHE_MAX_ENTRIES", cache.max_entries);
    set_u32("LDNS_CACHE_PEER_TTL_CAP", cache.peer_ttl_cap);
    set_u32("LDNS_CACHE_UPSTREAM_TTL_CAP", cache.upstream_ttl_cap);
    set_u32("LDNS_CACHE_NEGATIVE_TTL", cache.negative_ttl);

    set_u32("LDNS_DOMAINS_DEFAULT_TTL", domains.default_ttl);
    set_bool("LDNS_DOMAINS_WARN_PUBLIC_TLD", domains.warn_public_tld);

    set_bool("LDNS_SYSTEM_MANAGE_OS_DNS", system.manage_os_dns);
    set_bool("LDNS_SYSTEM_RESTORE_ON_EXIT", system.restore_on_exit);

    set_string("LDNS_UI_LANGUAGE", ui.language);
    set_string("LDNS_UI_THEME", ui.theme);
}

Result<void> Config::validate() const {
    if (network.name.empty()) return Error{510, "network.name must not be empty"};
    if (network.name.size() > 63) return Error{510, "network.name is longer than 63 bytes"};
    for (const char ch : network.name) {
        const auto c = static_cast<unsigned char>(ch);
        if (std::isspace(c) != 0 || c < 0x20) {
            return Error{510, "network.name contains whitespace"};
        }
    }
    if (network.discovery_port == 0) return Error{511, "network.discovery_port must be 1-65535"};
    if (network.sync_port == 0) return Error{511, "network.sync_port must be 1-65535"};
    // Both are bound by this process; UDP discovery and the TCP sync port are
    // different protocols so a shared number technically works, but it is
    // almost always a typo and makes the config ambiguous.
    if (network.discovery_port == network.sync_port) {
        return Error{511,
                     "network.discovery_port and network.sync_port must not be identical"};
    }
    if (server.listen.empty()) return Error{512, "server.listen must not be empty"};
    if (server.rate_limit_qps == 0) return Error{512, "server.rate_limit_qps must be positive"};
    if (peers.hedge_ms > peers.budget_ms) {
        return Error{513, "peers.hedge_ms must not exceed peers.budget_ms"};
    }
    if (domains.default_ttl == 0 || domains.default_ttl > 2'147'483'647u) {
        return Error{514, "domains.default_ttl is out of range"};
    }
    if (ui.language != "auto" && ui.language != "vi" && ui.language != "en") {
        return Error{515, "ui.language must be auto, vi or en"};
    }
    return {};
}

}  // namespace ldns::config
