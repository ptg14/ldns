#include "core/domain/self_address.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>

#include "core/dns/message.hpp"
#include "core/net/interfaces.hpp"

namespace ldns::domain {
namespace {

bool is_link_local_v4(const std::vector<std::uint8_t>& bytes) {
    return bytes.size() == 4 && bytes[0] == 169 && bytes[1] == 254;
}

bool is_loopback_v4(const std::vector<std::uint8_t>& bytes) {
    return bytes.size() == 4 && bytes[0] == 127;
}

std::vector<std::string> split_interfaces(std::string_view text) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t separator = text.find_first_of(", ", start);
        const std::size_t end = separator == std::string_view::npos ? text.size() : separator;
        if (end > start) out.emplace_back(text.substr(start, end - start));
        start = end + 1;
    }
    return out;
}

}  // namespace

Result<std::vector<AddressBytes>> SelfAddress::enumerate(const SelfAddressOptions& options) {
    if (!options.pinned_ip.empty()) {
        const auto parsed = IpAddress::parse(options.pinned_ip);
        if (!parsed) {
            return Error{600, "network.self_ip is not a valid address: " + options.pinned_ip};
        }
        const std::size_t size = parsed.value().v6 ? 16 : 4;
        return std::vector<AddressBytes>{AddressBytes(parsed.value().bytes, parsed.value().bytes + size)};
    }

    auto interfaces = net::list_interfaces();
    if (!interfaces) return interfaces.error();

    const std::vector<std::string> filter =
        options.interface_filter == "auto" || options.interface_filter.empty()
            ? std::vector<std::string>{}
            : split_interfaces(options.interface_filter);

    std::vector<AddressBytes> out;
    bool have_preferred{false};
    AddressBytes preferred;
    const bool explicit_interface = !options.interface_name.empty() || !filter.empty();
    for (const auto& interface : interfaces.value()) {
        if (!interface.up || interface.loopback) continue;
        // Virtual adapters (Docker, Hyper-V, VPN tunnels) are excluded per
        // SPEC §9.2 unless the user pinned an interface or an allowlist.
        if (interface.virtual_adapter && !explicit_interface) continue;
        if (!options.interface_name.empty() && interface.name != options.interface_name) continue;
        if (!filter.empty() &&
            std::find(filter.begin(), filter.end(), interface.name) == filter.end()) {
            continue;
        }
        for (const auto& address : interface.addresses) {
            // IPv4 only by default (see self_address.hpp). A pinned IPv6
            // address is still accepted through network.self_ip.
            if (!address.is_v4()) continue;
            if (is_loopback_v4(address.bytes) || is_link_local_v4(address.bytes)) continue;
            out.push_back(address.bytes);
            if (interface.default_route && !have_preferred) {
                preferred = address.bytes;
                have_preferred = true;
            }
        }
    }

    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());

    // Prefer the address that carries the default route (SPEC §9.2).
    if (have_preferred) {
        if (const auto it = std::find(out.begin(), out.end(), preferred); it != out.end()) {
            std::rotate(out.begin(), it, it + 1);
        }
    }

    if (out.empty() && !explicit_interface) {
        // Fallback: If no non-virtual adapter was found (e.g. running inside a cloud
        // VM, CI runner, or container), fall back to any active UP adapter carrying
        // the default route.
        for (const auto& interface : interfaces.value()) {
            if (!interface.up || interface.loopback || !interface.default_route) continue;
            for (const auto& address : interface.addresses) {
                if (!address.is_v4()) continue;
                if (is_loopback_v4(address.bytes) || is_link_local_v4(address.bytes)) continue;
                out.push_back(address.bytes);
            }
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
    }

    if (out.empty()) {
        return Error{602, "no LAN address found; set network.self_ip to pin one"};
    }
    return out;
}

}  // namespace ldns::domain
