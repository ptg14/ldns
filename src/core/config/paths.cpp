#include "core/config/paths.hpp"

#include <cstdlib>
#include <filesystem>

namespace ldns::config {
namespace {

namespace fs = std::filesystem;

std::string getenv_or(std::string_view name, std::string_view fallback) {
    const std::string key(name);
#if defined(_WIN32)
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, key.c_str()) == 0 && value != nullptr && *value != '\0') {
        std::string out(value);
        std::free(value);
        return out;
    }
    std::free(value);
#else
    if (const char* value = std::getenv(key.c_str()); value != nullptr && *value != '\0') {
        return value;
    }
#endif
    return std::string(fallback);
}

std::string home_dir() {
#if defined(_WIN32)
    return getenv_or("USERPROFILE", "");
#else
    return getenv_or("HOME", "");
#endif
}

}  // namespace

std::string expand_home(std::string path) {
    if (path.size() < 2 || path[0] != '~' || (path[1] != '/' && path[1] != '\\')) return path;
    const std::string home = home_dir();
    if (home.empty()) return path;
    return home + path.substr(1);
}

Paths default_paths(InstallKind kind) {
    Paths out;

#if defined(_WIN32)
    if (kind == InstallKind::Service) {
        const std::string base = getenv_or("ProgramData", "C:\\ProgramData");
        out.config_file = base + "\\ldns\\config.toml";
        out.data_file = base + "\\ldns\\ldns.db";
    } else {
        const std::string base = getenv_or("APPDATA", home_dir() + "\\AppData\\Roaming");
        out.config_file = base + "\\ldns\\config.toml";
        out.data_file = base + "\\ldns\\ldns.db";
    }
#else
    if (kind == InstallKind::Service) {
        out.config_file = "/etc/ldns/config.toml";
        out.data_file = "/var/lib/ldns/ldns.db";
    } else {
        const std::string config_home =
            getenv_or("XDG_CONFIG_HOME", home_dir() + "/.config");
        const std::string data_home =
            getenv_or("XDG_DATA_HOME", home_dir() + "/.local/share");
        out.config_file = config_home + "/ldns/config.toml";
        out.data_file = data_home + "/ldns/ldns.db";
    }
#endif

    if (const std::string override_file = getenv_or("LDNS_CONFIG", ""); !override_file.empty()) {
        out.config_file = override_file;
    }
    if (const std::string override_data = getenv_or("LDNS_DATA", ""); !override_data.empty()) {
        out.data_file = override_data;
    }
    return out;
}

}  // namespace ldns::config
