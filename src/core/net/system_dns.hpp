#pragma once

// System DNS discovery (SPEC §6, §11, PLAN M3).
//
// When upstream.servers is set to "auto" (default), Ldns queries the operating
// system to discover currently configured DNS servers:
//   - Windows: queries GetAdaptersAddresses (FirstDnsServerAddress), prioritizing
//     the adapter carrying the default route.
//   - Linux: reads /run/systemd/resolve/resolv.conf (if systemd-resolved is active)
//     or /etc/resolv.conf (extracting "nameserver <ip>" lines).

#include <string>
#include <vector>

#include "core/util/result.hpp"

namespace ldns::net {

struct SystemDnsServers {
    std::vector<std::string> servers;
};

// Detects the active DNS servers configured in the operating system.
// Returns an empty list if none could be found (caller should use fallback).
Result<SystemDnsServers> detect_system_dns();

}  // namespace ldns::net
