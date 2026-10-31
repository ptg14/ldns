#pragma once

#include <string>
#include <vector>

#include "core/core_api.hpp"
#include "core/util/result.hpp"

namespace ldns::os {

struct SystemDnsStatus {
    bool is_managed_by_ldns{false};
    std::vector<std::string> current_servers;
    std::vector<std::string> backup_servers;
    std::string method_used;
};

class SystemDnsManager {
public:
    explicit SystemDnsManager(CoreApi& api);

    /// Configures OS DNS to route through 127.0.0.1 (SPEC §13.2).
    /// Always creates a backup of the original servers first.
    Result<void> setup();

    /// Restores OS DNS to the saved backup settings.
    Result<void> restore();

    /// Returns current OS DNS status and backup state.
    SystemDnsStatus status() const;

private:
    CoreApi& api_;
};

} // namespace ldns::os
