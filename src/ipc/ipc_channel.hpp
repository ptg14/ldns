#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>

#include "core/config/paths.hpp"

namespace ldns::ipc {

inline std::string default_ipc_endpoint(config::InstallKind kind = config::InstallKind::User) {
#ifdef _WIN32
    if (kind == config::InstallKind::Service) {
        return "\\\\.\\pipe\\ldns-service";
    }
    return "\\\\.\\pipe\\ldns";
#else
    if (kind == config::InstallKind::Service) {
        return "/run/ldns/ldns.sock";
    }
    const char* xdg = std::getenv("XDG_RUNTIME_DIR");
    if (xdg != nullptr && xdg[0] != '\0') {
        return (std::filesystem::path(xdg) / "ldns.sock").string();
    }
    const char* user = std::getenv("USER");
    if (user != nullptr && user[0] != '\0') {
        return "/tmp/ldns-" + std::string(user) + ".sock";
    }
    return "/tmp/ldns.sock";
#endif
}

} // namespace ldns::ipc
