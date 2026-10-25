#include "core/net/interfaces.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <string_view>

#include <asio.hpp>

#include "core/dns/message.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <cerrno>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace ldns::net {
namespace {

#if defined(_WIN32)
std::string narrow_utf16(const wchar_t* text) {
    if (text == nullptr) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) return {};
    std::string out(static_cast<std::size_t>(needed - 1), char{0});
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), needed, nullptr, nullptr);
    return out;
}
#endif

bool looks_virtual(std::string_view name) {
    static constexpr std::array<std::string_view, 10> kVirtualPrefixes = {
        "docker", "veth",    "virbr", "br-",   "vmnet", "vboxnet",
        "tun",    "tap",     "wg",    "zt",
    };
    std::string lowered(name);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    for (const auto prefix : kVirtualPrefixes) {
        if (lowered.starts_with(prefix)) return true;
    }
#if defined(_WIN32)
    // Windows adapter descriptions carry these markers rather than names.
    for (const std::string_view marker : {"vethernet", "hyper-v", "virtualbox", "vmware",
                                          "loopback", "wsl", "tap-windows", "tailscale",
                                          "zerotier", "wireguard"}) {
        if (lowered.find(std::string(marker)) != std::string::npos) return true;
    }
#endif
    return false;
}

std::vector<std::uint8_t> default_route_address_impl() {
    asio::io_context io;
    asio::error_code ec;
    asio::ip::udp::socket socket(io);
    socket.open(asio::ip::udp::v4(), ec);
    if (ec) return {};
    // TEST-NET-1 (RFC 5737) is never a real destination; the connect only
    // performs a route lookup and selects the source address.
    socket.connect(asio::ip::udp::endpoint(asio::ip::make_address_v4("192.0.2.1"), 53), ec);
    if (ec) return {};
    const auto local = socket.local_endpoint(ec);
    if (ec) return {};
    const auto bytes = local.address().to_v4().to_bytes();
    return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
}

#if defined(_WIN32)
Result<std::vector<InterfaceInfo>> list_interfaces_impl() {
    ULONG size = 15000;
    std::vector<std::uint8_t> buffer(size);
    auto* addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());

    constexpr ULONG kFlags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                             GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_PREFIX;
    ULONG status = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3 && status == ERROR_BUFFER_OVERFLOW; ++attempt) {
        status = GetAdaptersAddresses(AF_UNSPEC, kFlags, nullptr, addresses, &size);
        if (status == ERROR_BUFFER_OVERFLOW) buffer.resize(size);
        addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    }
    if (status != NO_ERROR) {
        return Error{601, "GetAdaptersAddresses failed: " + std::to_string(status)};
    }

    std::vector<InterfaceInfo> out;
    for (auto* adapter = addresses; adapter != nullptr; adapter = adapter->Next) {
        InterfaceInfo info;
        // FriendlyName is UTF-16; convert without pulling in a codec library.
        info.name = narrow_utf16(adapter->FriendlyName);
        info.up = adapter->OperStatus == IfOperStatusUp;
        info.loopback = adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK;
        // Vendor/driver markers (Hyper-V, VMware, ...) usually live in
        // Description rather than FriendlyName.
        const std::string description = narrow_utf16(adapter->Description);
        info.virtual_adapter = looks_virtual(info.name) || looks_virtual(description);

        for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr;
             unicast = unicast->Next) {
            if (unicast->Address.lpSockaddr == nullptr) continue;
            if (unicast->Address.lpSockaddr->sa_family == AF_INET) {
                const auto* v4 = reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
                const auto* raw = reinterpret_cast<const std::uint8_t*>(&v4->sin_addr.s_addr);
                info.addresses.push_back(InterfaceAddress{{raw[0], raw[1], raw[2], raw[3]}});
            } else if (unicast->Address.lpSockaddr->sa_family == AF_INET6) {
                const auto* v6 = reinterpret_cast<const sockaddr_in6*>(unicast->Address.lpSockaddr);
                const auto* raw = reinterpret_cast<const std::uint8_t*>(&v6->sin6_addr.s6_addr);
                info.addresses.push_back(
                    InterfaceAddress{std::vector<std::uint8_t>(raw, raw + 16)});
            }
        }
        out.push_back(std::move(info));
    }
    return out;
}
#else
Result<std::vector<InterfaceInfo>> list_interfaces_impl() {
    struct ifaddrs* head = nullptr;
    if (getifaddrs(&head) != 0) {
        return Error{601, std::string("getifaddrs failed: ") + std::strerror(errno)};
    }

    std::vector<InterfaceInfo> out;
    auto find = [&](const std::string& name) -> InterfaceInfo& {
        const auto it = std::find_if(out.begin(), out.end(),
                                     [&](const InterfaceInfo& item) { return item.name == name; });
        if (it != out.end()) return *it;
        out.push_back(InterfaceInfo{name, {}, false, false, looks_virtual(name), false});
        return out.back();
    };

    for (auto* entry = head; entry != nullptr; entry = entry->ifa_next) {
        if (entry->ifa_name == nullptr || entry->ifa_addr == nullptr) continue;
        InterfaceInfo& info = find(entry->ifa_name);
        info.up = info.up || (entry->ifa_flags & IFF_UP) != 0;
        info.loopback = info.loopback || (entry->ifa_flags & IFF_LOOPBACK) != 0;

        if (entry->ifa_addr->sa_family == AF_INET) {
            const auto* v4 = reinterpret_cast<const sockaddr_in*>(entry->ifa_addr);
            const auto* raw = reinterpret_cast<const std::uint8_t*>(&v4->sin_addr.s_addr);
            info.addresses.push_back(InterfaceAddress{{raw[0], raw[1], raw[2], raw[3]}});
        } else if (entry->ifa_addr->sa_family == AF_INET6) {
            const auto* v6 = reinterpret_cast<const sockaddr_in6*>(entry->ifa_addr);
            const auto* raw = reinterpret_cast<const std::uint8_t*>(&v6->sin6_addr.s6_addr);
            info.addresses.push_back(InterfaceAddress{std::vector<std::uint8_t>(raw, raw + 16)});
        }
    }
    freeifaddrs(head);
    return out;
}
#endif

}  // namespace

std::string InterfaceAddress::to_string() const {
    if (is_v4()) return dns::format_ipv4(bytes);
    if (is_v6()) return dns::format_ipv6(bytes);
    return {};
}

Result<std::vector<InterfaceInfo>> list_interfaces() {
    auto result = list_interfaces_impl();
    if (!result) return result;

    const auto preferred = default_route_address_impl();
    auto& interfaces = result.value();
    if (!preferred.empty()) {
        for (auto& info : interfaces) {
            for (const auto& address : info.addresses) {
                if (address.bytes == preferred) {
                    info.default_route = true;
                }
            }
        }
    }
    return result;
}

std::vector<std::uint8_t> default_route_address() { return default_route_address_impl(); }

}  // namespace ldns::net
