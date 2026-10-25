#pragma once

// Platform network-interface enumeration (SPEC §9.2, PLAN S2).
//
// Standalone Asio has no interface enumeration API, so this wraps the OS
// calls: GetAdaptersAddresses on Windows, getifaddrs on Linux/macOS. Only the
// fields the resolver and discovery need are exposed.

#include <cstdint>
#include <string>
#include <vector>

#include "core/util/result.hpp"

namespace ldns::net {

struct InterfaceAddress {
    // 4 bytes (IPv4) or 16 bytes (IPv6).
    std::vector<std::uint8_t> bytes;

    bool is_v4() const noexcept { return bytes.size() == 4; }
    bool is_v6() const noexcept { return bytes.size() == 16; }
    std::string to_string() const;

    friend bool operator==(const InterfaceAddress& a, const InterfaceAddress& b) noexcept {
        return a.bytes == b.bytes;
    }
    friend bool operator<(const InterfaceAddress& a, const InterfaceAddress& b) noexcept {
        return a.bytes < b.bytes;
    }
};

struct InterfaceInfo {
    std::string name;
    std::vector<InterfaceAddress> addresses;
    bool up{false};
    bool loopback{false};
    // True for virtual adapters (VPN, Hyper-V, Docker, WSL) that SPEC §9.2
    // excludes from `self` by default.
    bool virtual_adapter{false};
    // The adapter that carries the default route, as reported by the OS.
    bool default_route{false};
};

// All interfaces with at least one address. Errors carry code 601.
Result<std::vector<InterfaceInfo>> list_interfaces();

// Best-effort default-route address (IPv4). Returns an empty vector when no
// route exists (offline, captive portal, unusual routing).
std::vector<std::uint8_t> default_route_address();

}  // namespace ldns::net
