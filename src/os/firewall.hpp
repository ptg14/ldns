#pragma once

#include <string>
#include <vector>

#include "core/util/result.hpp"

namespace ldns::os {

struct FirewallStatus {
    bool has_dns_rule{false};
    bool has_discovery_rule{false};
    bool has_sync_rule{false};
    std::string backend_detected; // "windows_advfirewall", "ufw", "firewalld", "none"
};

class Firewall {
public:
    /// Inspects system firewall configuration for Ldns ports (53, 5390, 5391).
    static FirewallStatus status();

    /// Adds firewall rules to allow local LAN traffic for Ldns.
    static Result<void> add_rules();

    /// Removes Ldns firewall rules.
    static Result<void> remove_rules();
};

} // namespace ldns::os
