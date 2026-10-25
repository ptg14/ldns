#pragma once

// Default configuration and data locations (SPEC §12).
//
// Linux (user):    ~/.config/ldns/config.toml   ~/.local/share/ldns/ldns.db
// Linux (service): /etc/ldns/config.toml        /var/lib/ldns/ldns.db
// Windows (user):  %APPDATA%\ldns\config.toml   %APPDATA%\ldns\ldns.db
// Windows (svc):   %ProgramData%\ldns\...       %ProgramData%\ldns\ldns.db
//
// Service locations are selected explicitly (the service runs as a dedicated
// user), never by guessing at privileges.

#include <string>

#include "core/util/result.hpp"

namespace ldns::config {

enum class InstallKind { User, Service };

struct Paths {
    std::string config_file;
    std::string data_file;
};

// Resolves the locations for this process. LDNS_CONFIG and LDNS_DATA override
// the file paths (used by tests and by `--config`); empty means default.
Paths default_paths(InstallKind kind = InstallKind::User);

// Expands a leading "~" to $HOME (POSIX) or %USERPROFILE% (Windows). Other
// paths are returned unchanged.
std::string expand_home(std::string path);

}  // namespace ldns::config
