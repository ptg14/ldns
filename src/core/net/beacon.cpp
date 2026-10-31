#include "core/net/beacon.hpp"
#include "core/net/crypto.hpp"

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace ldns::net {

namespace {

constexpr std::array<std::uint8_t, 4> kMagic = {'L', 'D', 'B', '1'};

}  // namespace

Result<std::vector<std::uint8_t>> encode_beacon(const Beacon& beacon,
                                                std::span<const std::uint8_t> k_beacon) {
    json j;
    j["t"] = beacon.type;
    j["v"] = json::array({beacon.version[0], beacon.version[1]});

    std::vector<std::uint8_t> node_bytes(beacon.node_id.begin(), beacon.node_id.end());
    j["node"] = json::binary(std::move(node_bytes));

    j["name"] = beacon.name;

    std::vector<std::uint8_t> net_bytes(beacon.net.begin(), beacon.net.end());
    j["net"] = json::binary(std::move(net_bytes));

    j["sync_port"] = beacon.sync_port;
    j["dns_port"] = beacon.dns_port;

    json ips_arr = json::array();
    for (const auto& ip : beacon.ips) {
        ips_arr.push_back(json::binary(ip));
    }
    j["ips"] = std::move(ips_arr);

    j["epoch"] = beacon.epoch;
    j["seq"] = beacon.seq;
    j["ts"] = beacon.ts;

    std::vector<std::uint8_t> body;
    json::to_msgpack(j, body);

    const bool has_mac = !k_beacon.empty();
    const std::uint8_t flags = has_mac ? 0x01 : 0x00;
    const std::size_t mac_size = has_mac ? 16 : 0;

    // protocol.md §3.1: "LDB1" (4B) + flags (1B) + body + optional mac (16B)
    if (5 + body.size() + mac_size > kMaxBeaconDatagramSize) {
        return Error{802, "beacon datagram exceeds 1200 bytes limit"};
    }

    std::vector<std::uint8_t> out;
    out.reserve(5 + body.size() + mac_size);
    out.insert(out.end(), kMagic.begin(), kMagic.end());
    out.push_back(flags);
    out.insert(out.end(), body.begin(), body.end());

    if (has_mac) {
        auto mac = crypto::compute_beacon_mac(k_beacon, flags, body);
        out.insert(out.end(), mac.begin(), mac.end());
    }

    return out;
}

Result<Beacon> decode_beacon(std::span<const std::uint8_t> packet,
                             std::span<const std::uint8_t> k_beacon) {
    if (packet.size() > kMaxBeaconDatagramSize) {
        return Error{803, "beacon datagram exceeds maximum size of 1200 bytes"};
    }
    if (packet.size() < 5) {
        return Error{804, "beacon datagram too short"};
    }

    if (!std::equal(kMagic.begin(), kMagic.end(), packet.begin())) {
        return Error{805, "invalid beacon magic"};
    }

    const std::uint8_t flags = packet[4];
    const bool has_mac = (flags & 0x01) != 0;
    const std::size_t mac_size = has_mac ? 16 : 0;

    if (packet.size() < 5 + mac_size) {
        return Error{804, "beacon datagram too short for flags"};
    }

    const std::size_t body_size = packet.size() - 5 - mac_size;
    auto body_span = packet.subspan(5, body_size);

    if (!k_beacon.empty()) {
        if (!has_mac) {
            return Error{808, "beacon missing required MAC on PSK network"};
        }
        std::array<std::uint8_t, 16> mac{};
        std::copy_n(packet.data() + packet.size() - 16, 16, mac.begin());
        if (!crypto::verify_beacon_mac(k_beacon, flags, body_span, mac)) {
            return Error{809, "beacon MAC verification failed"};
        }
    }

    auto j = json::from_msgpack(body_span.begin(), body_span.end(), true, false);
    if (j.is_discarded() || !j.is_object()) {
        return Error{806, "failed to parse beacon messagepack body"};
    }

    // Required fields per protocol.md §3.2
    if (!j.contains("t") || !j["t"].is_string()) {
        return Error{807, "beacon missing type 't'"};
    }
    if (!j.contains("v") || !j["v"].is_array() || j["v"].size() != 2) {
        return Error{807, "beacon missing or invalid version 'v'"};
    }
    if (!j.contains("node") || !j["node"].is_binary() || j["node"].get_binary().size() != 16) {
        return Error{807, "beacon missing or invalid 'node'"};
    }
    if (!j.contains("name") || !j["name"].is_string()) {
        return Error{807, "beacon missing 'name'"};
    }
    if (!j.contains("net") || !j["net"].is_binary() || j["net"].get_binary().size() != 8) {
        return Error{807, "beacon missing or invalid 'net'"};
    }

    Beacon b;
    b.type = j["t"].get<std::string>();
    b.version = {j["v"][0].get<std::uint8_t>(), j["v"][1].get<std::uint8_t>()};

    const auto& node_bin = j["node"].get_binary();
    std::copy_n(node_bin.begin(), 16, b.node_id.begin());

    b.name = j["name"].get<std::string>();

    const auto& net_bin = j["net"].get_binary();
    std::copy_n(net_bin.begin(), 8, b.net.begin());

    if (j.contains("sync_port") && j["sync_port"].is_number_unsigned()) {
        b.sync_port = j["sync_port"].get<std::uint16_t>();
    }
    if (j.contains("dns_port") && j["dns_port"].is_number_unsigned()) {
        b.dns_port = j["dns_port"].get<std::uint16_t>();
    }

    if (j.contains("ips") && j["ips"].is_array()) {
        for (const auto& item : j["ips"]) {
            if (item.is_binary()) {
                const auto& bin = item.get_binary();
                b.ips.emplace_back(bin.begin(), bin.end());
            }
        }
    }

    if (j.contains("epoch") && j["epoch"].is_number_unsigned()) {
        b.epoch = j["epoch"].get<std::uint64_t>();
    }
    if (j.contains("seq") && j["seq"].is_number_unsigned()) {
        b.seq = j["seq"].get<std::uint64_t>();
    }
    if (j.contains("ts") && j["ts"].is_number_unsigned()) {
        b.ts = j["ts"].get<std::uint64_t>();
    }

    return b;
}

}  // namespace ldns::net
