#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "core/util/id.hpp"
#include "core/util/result.hpp"

namespace ldns::net {

constexpr std::uint16_t kDiscoveryPort = 5390;
constexpr std::string_view kDiscoveryMulticastIpv4 = "239.76.68.83";
constexpr std::size_t kMaxBeaconDatagramSize = 1200;

struct Beacon {
    std::string type{"beacon"};               // "beacon" or "bye"
    std::array<std::uint8_t, 2> version{1, 0}; // [major, minor]
    NodeId node_id{};
    std::string name;
    std::array<std::uint8_t, 8> net{};         // 8-byte network hash
    std::uint16_t sync_port{5391};
    std::uint16_t dns_port{53};
    std::vector<std::vector<std::uint8_t>> ips; // 4-byte (IPv4) or 16-byte (IPv6)
    std::uint64_t epoch{0};
    std::uint64_t seq{0};
    std::uint64_t ts{0};                       // HLC timestamp
};

// Encodes a Beacon struct into wire format according to protocol.md §3.
Result<std::vector<std::uint8_t>> encode_beacon(const Beacon& beacon);

// Decodes a raw datagram into a Beacon struct according to protocol.md §3.
Result<Beacon> decode_beacon(std::span<const std::uint8_t> packet);

}  // namespace ldns::net
