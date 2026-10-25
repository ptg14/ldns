#pragma once

// Addresses a `self` record publishes (SPEC §5.2, §9.2).
//
// The set is: unicast IPv4 addresses of non-loopback, non-link-local
// interfaces on the Ldns network (preferring the interface that carries the
// default route), or the pinned self_ip. IPv6 is not published in v1: a
// machine usually has several volatile addresses per interface and publishing
// all of them makes `self` records churn (SPEC §9.2 says "tập IP của self =
// địa chỉ IPv4 unicast"; IPv6 entries are picked up when explicit self_ip is
// pinned).
//
// Enumeration uses the OS socket API through Asio, which keeps the same code
// on Windows and Linux.

#include <cstdint>
#include <string>
#include <vector>

#include "core/server/acl.hpp"
#include "core/util/result.hpp"

namespace ldns::domain {

// One published address: 4 bytes for IPv4, 16 for IPv6.
using AddressBytes = std::vector<std::uint8_t>;

struct SelfAddressOptions {
    // Pinned address; when non-empty it is the only published value.
    std::string pinned_ip;
    // Pinned interface; restricts enumeration to that interface.
    std::string interface_name;
    // "auto" or a comma/space separated list from network.interfaces.
    std::string interface_filter{"auto"};
};

class SelfAddress {
public:
    // Enumerates addresses. Fails when `pinned_ip` is invalid or no usable
    // address exists (the caller reports "no LAN address, pin self_ip").
    static Result<std::vector<AddressBytes>> enumerate(const SelfAddressOptions& options);

};

}  // namespace ldns::domain
