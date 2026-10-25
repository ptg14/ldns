#include "core/net/system_dns.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>

#include "core/dns/message.hpp"
#include "core/net/interfaces.hpp"

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
#endif

namespace ldns::net {
namespace {

std::string trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    return std::string(text);
}

#if defined(_WIN32)
Result<SystemDnsServers> detect_windows_dns() {
    ULONG size = 15000;
    std::vector<std::uint8_t> buffer(size);
    auto* addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());

    // Do NOT pass GAA_FLAG_SKIP_DNS_SERVER so we get DNS servers.
    constexpr ULONG kFlags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_INCLUDE_PREFIX;
    ULONG status = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3 && status == ERROR_BUFFER_OVERFLOW; ++attempt) {
        status = GetAdaptersAddresses(AF_UNSPEC, kFlags, nullptr, addresses, &size);
        if (status == ERROR_BUFFER_OVERFLOW) buffer.resize(size);
        addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    }
    if (status != NO_ERROR) {
        return Error{610, "GetAdaptersAddresses failed: " + std::to_string(status)};
    }

    const auto preferred_route = default_route_address();
    std::vector<std::string> preferred_dns;
    std::vector<std::string> other_dns;

    for (auto* adapter = addresses; adapter != nullptr; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }

        bool has_default_route = false;
        if (!preferred_route.empty()) {
            for (auto* u = adapter->FirstUnicastAddress; u != nullptr; u = u->Next) {
                if (u->Address.lpSockaddr == nullptr) continue;
                if (u->Address.lpSockaddr->sa_family == AF_INET) {
                    const auto* v4 = reinterpret_cast<const sockaddr_in*>(u->Address.lpSockaddr);
                    const auto* raw = reinterpret_cast<const std::uint8_t*>(&v4->sin_addr.s_addr);
                    if (preferred_route.size() == 4 &&
                        std::equal(preferred_route.begin(), preferred_route.end(), raw)) {
                        has_default_route = true;
                        break;
                    }
                }
            }
        }

        for (auto* dns = adapter->FirstDnsServerAddress; dns != nullptr; dns = dns->Next) {
            if (dns->Address.lpSockaddr == nullptr) continue;
            std::string ip_str;
            if (dns->Address.lpSockaddr->sa_family == AF_INET) {
                const auto* v4 = reinterpret_cast<const sockaddr_in*>(dns->Address.lpSockaddr);
                const auto* raw = reinterpret_cast<const std::uint8_t*>(&v4->sin_addr.s_addr);
                ip_str = dns::format_ipv4(std::span<const std::uint8_t>(raw, 4));
            } else if (dns->Address.lpSockaddr->sa_family == AF_INET6) {
                const auto* v6 = reinterpret_cast<const sockaddr_in6*>(dns->Address.lpSockaddr);
                const auto* raw = reinterpret_cast<const std::uint8_t*>(&v6->sin6_addr.s6_addr);
                ip_str = dns::format_ipv6(std::span<const std::uint8_t>(raw, 16));
            }
            if (!ip_str.empty()) {
                if (has_default_route) {
                    preferred_dns.push_back(ip_str);
                } else {
                    other_dns.push_back(ip_str);
                }
            }
        }
    }

    SystemDnsServers out;
    for (auto& ip : preferred_dns) {
        if (std::find(out.servers.begin(), out.servers.end(), ip) == out.servers.end()) {
            out.servers.push_back(std::move(ip));
        }
    }
    for (auto& ip : other_dns) {
        if (std::find(out.servers.begin(), out.servers.end(), ip) == out.servers.end()) {
            out.servers.push_back(std::move(ip));
        }
    }
    return out;
}
#else
Result<SystemDnsServers> detect_linux_dns() {
    namespace fs = std::filesystem;
    // Prefer systemd-resolved's actual upstream resolv.conf if available to avoid
    // hitting the 127.0.0.53 stub if we become the primary resolver.
    const char* path = "/etc/resolv.conf";
    if (fs::exists("/run/systemd/resolve/resolv.conf")) {
        path = "/run/systemd/resolve/resolv.conf";
    }

    std::ifstream file(path);
    if (!file) {
        return Error{611, std::string("cannot open ") + path};
    }

    SystemDnsServers out;
    std::string line;
    while (std::getline(file, line)) {
        std::string_view sv = line;
        while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front()))) sv.remove_prefix(1);
        if (sv.starts_with("#") || sv.starts_with(";")) continue;
        if (sv.starts_with("nameserver")) {
            sv.remove_prefix(10);
            std::string ip = trim(sv);
            if (!ip.empty()) {
                if (std::find(out.servers.begin(), out.servers.end(), ip) == out.servers.end()) {
                    out.servers.push_back(std::move(ip));
                }
            }
        }
    }
    return out;
}
#endif

}  // namespace

Result<SystemDnsServers> detect_system_dns() {
#if defined(_WIN32)
    return detect_windows_dns();
#else
    return detect_linux_dns();
#endif
}

}  // namespace ldns::net
