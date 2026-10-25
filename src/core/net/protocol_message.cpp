#include "core/net/protocol_message.hpp"

#include <nlohmann/json.hpp>
#include <algorithm>

using json = nlohmann::json;

namespace ldns::net {

std::string_view error_code_name(ProtocolErrorCode code) {
    switch (code) {
        case ProtocolErrorCode::Version: return "VERSION";
        case ProtocolErrorCode::Network: return "NETWORK";
        case ProtocolErrorCode::Auth: return "AUTH";
        case ProtocolErrorCode::Malformed: return "MALFORMED";
        case ProtocolErrorCode::Busy: return "BUSY";
        case ProtocolErrorCode::TooManyPeers: return "TOO_MANY_PEERS";
        case ProtocolErrorCode::Self: return "SELF";
        case ProtocolErrorCode::Duplicate: return "DUPLICATE";
        case ProtocolErrorCode::NotAllowed: return "NOT_ALLOWED";
    }
    return "UNKNOWN";
}

Result<std::vector<std::uint8_t>> frame_payload(std::span<const std::uint8_t> payload) {
    if (payload.size() > kMaxFrameSize) {
        return Error{820, "frame payload exceeds 4 MiB maximum limit"};
    }

    std::vector<std::uint8_t> out;
    out.reserve(4 + payload.size());

    const std::uint32_t len = static_cast<std::uint32_t>(payload.size());
    out.push_back(static_cast<std::uint8_t>((len >> 24) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((len >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((len >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(len & 0xFF));

    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

Result<std::vector<std::uint8_t>> encode_hello_frame(const HelloMessage& msg) {
    json j;
    j["t"] = "HELLO";
    j["v"] = json::array({msg.version[0], msg.version[1]});

    std::vector<std::uint8_t> node_bytes(msg.node_id.begin(), msg.node_id.end());
    j["node"] = json::binary(std::move(node_bytes));

    j["name"] = msg.name;

    std::vector<std::uint8_t> net_bytes(msg.net.begin(), msg.net.end());
    j["net"] = json::binary(std::move(net_bytes));

    j["epoch"] = msg.epoch;
    j["seq"] = msg.seq;

    json ips_arr = json::array();
    for (const auto& ip : msg.ips) {
        ips_arr.push_back(json::binary(ip));
    }
    j["ips"] = std::move(ips_arr);

    j["sync_port"] = msg.sync_port;
    j["dns_port"] = msg.dns_port;
    j["caps"] = msg.caps;

    std::vector<std::uint8_t> nonce_bytes(msg.nonce.begin(), msg.nonce.end());
    j["nonce"] = json::binary(std::move(nonce_bytes));

    j["psk"] = msg.psk;

    std::vector<std::uint8_t> payload;
    json::to_msgpack(j, payload);

    return frame_payload(payload);
}

Result<std::vector<std::uint8_t>> encode_ping_frame() {
    json j;
    j["t"] = "PING";
    std::vector<std::uint8_t> payload;
    json::to_msgpack(j, payload);
    return frame_payload(payload);
}

Result<std::vector<std::uint8_t>> encode_pong_frame() {
    json j;
    j["t"] = "PONG";
    std::vector<std::uint8_t> payload;
    json::to_msgpack(j, payload);
    return frame_payload(payload);
}

Result<std::vector<std::uint8_t>> encode_bye_frame() {
    json j;
    j["t"] = "BYE";
    std::vector<std::uint8_t> payload;
    json::to_msgpack(j, payload);
    return frame_payload(payload);
}

Result<std::vector<std::uint8_t>> encode_peers_frame(const PeersMessage& msg) {
    json j;
    j["t"] = "PEERS";

    json peers_arr = json::array();
    for (const auto& p : msg.peers) {
        json item;
        std::vector<std::uint8_t> node_bytes(p.node_id.begin(), p.node_id.end());
        item["node"] = json::binary(std::move(node_bytes));
        json ips = json::array();
        for (const auto& ip : p.ips) ips.push_back(json::binary(ip));
        item["ips"] = std::move(ips);
        item["sync_port"] = p.sync_port;
        peers_arr.push_back(std::move(item));
    }
    j["peers"] = std::move(peers_arr);

    std::vector<std::uint8_t> payload;
    json::to_msgpack(j, payload);
    return frame_payload(payload);
}

Result<std::vector<std::uint8_t>> encode_error_frame(const ErrorMessage& msg) {
    json j;
    j["t"] = "ERROR";
    j["code"] = static_cast<std::uint8_t>(msg.code);
    j["msg"] = msg.msg;

    std::vector<std::uint8_t> payload;
    json::to_msgpack(j, payload);
    return frame_payload(payload);
}

Result<ParsedMessage> parse_payload(std::span<const std::uint8_t> payload) {
    auto j = json::from_msgpack(payload.begin(), payload.end(), true, false);
    if (j.is_discarded() || !j.is_object()) {
        return Error{821, "malformed messagepack map payload"};
    }

    if (!j.contains("t") || !j["t"].is_string()) {
        return Error{822, "missing or invalid message type 't'"};
    }

    ParsedMessage parsed;
    parsed.type_name = j["t"].get<std::string>();

    if (parsed.type_name == "HELLO") {
        parsed.type = MessageType::Hello;
        if (!j.contains("v") || !j["v"].is_array() || j["v"].size() != 2) {
            return Error{823, "invalid HELLO version 'v'"};
        }
        if (!j.contains("node") || !j["node"].is_binary() || j["node"].get_binary().size() != 16) {
            return Error{823, "invalid HELLO node_id"};
        }
        if (!j.contains("name") || !j["name"].is_string()) {
            return Error{823, "invalid HELLO name"};
        }
        if (!j.contains("net") || !j["net"].is_binary() || j["net"].get_binary().size() != 8) {
            return Error{823, "invalid HELLO net hash"};
        }

        HelloMessage h;
        h.version = {j["v"][0].get<std::uint8_t>(), j["v"][1].get<std::uint8_t>()};
        const auto& node_bin = j["node"].get_binary();
        std::copy_n(node_bin.begin(), 16, h.node_id.begin());
        h.name = j["name"].get<std::string>();
        const auto& net_bin = j["net"].get_binary();
        std::copy_n(net_bin.begin(), 8, h.net.begin());

        if (j.contains("epoch") && j["epoch"].is_number_unsigned()) h.epoch = j["epoch"].get<std::uint64_t>();
        if (j.contains("seq") && j["seq"].is_number_unsigned()) h.seq = j["seq"].get<std::uint64_t>();
        if (j.contains("sync_port") && j["sync_port"].is_number_unsigned()) h.sync_port = j["sync_port"].get<std::uint16_t>();
        if (j.contains("dns_port") && j["dns_port"].is_number_unsigned()) h.dns_port = j["dns_port"].get<std::uint16_t>();

        if (j.contains("ips") && j["ips"].is_array()) {
            for (const auto& item : j["ips"]) {
                if (item.is_binary()) {
                    const auto& bin = item.get_binary();
                    h.ips.emplace_back(bin.begin(), bin.end());
                }
            }
        }

        if (j.contains("caps") && j["caps"].is_array()) {
            for (const auto& item : j["caps"]) {
                if (item.is_string()) h.caps.push_back(item.get<std::string>());
            }
        }

        if (j.contains("nonce") && j["nonce"].is_binary() && j["nonce"].get_binary().size() == 32) {
            const auto& nonce_bin = j["nonce"].get_binary();
            std::copy_n(nonce_bin.begin(), 32, h.nonce.begin());
        }

        if (j.contains("psk") && j["psk"].is_boolean()) h.psk = j["psk"].get<bool>();

        parsed.hello = std::move(h);
    } else if (parsed.type_name == "PING") {
        parsed.type = MessageType::Ping;
    } else if (parsed.type_name == "PONG") {
        parsed.type = MessageType::Pong;
    } else if (parsed.type_name == "BYE") {
        parsed.type = MessageType::Bye;
    } else if (parsed.type_name == "ERROR") {
        parsed.type = MessageType::Error;
        ErrorMessage err;
        if (j.contains("code") && j["code"].is_number_unsigned()) {
            err.code = static_cast<ProtocolErrorCode>(j["code"].get<std::uint8_t>());
        }
        if (j.contains("msg") && j["msg"].is_string()) {
            err.msg = j["msg"].get<std::string>();
        }
        parsed.error = std::move(err);
    } else if (parsed.type_name == "PEERS") {
        parsed.type = MessageType::Peers;
        PeersMessage pm;
        if (j.contains("peers") && j["peers"].is_array()) {
            for (const auto& item : j["peers"]) {
                if (item.is_object() && item.contains("node") && item["node"].is_binary() &&
                    item["node"].get_binary().size() == 16) {
                    PeerItem pi;
                    const auto& nbin = item["node"].get_binary();
                    std::copy_n(nbin.begin(), 16, pi.node_id.begin());
                    if (item.contains("sync_port") && item["sync_port"].is_number_unsigned()) {
                        pi.sync_port = item["sync_port"].get<std::uint16_t>();
                    }
                    if (item.contains("ips") && item["ips"].is_array()) {
                        for (const auto& ip_item : item["ips"]) {
                            if (ip_item.is_binary()) {
                                const auto& ip_bin = ip_item.get_binary();
                                pi.ips.emplace_back(ip_bin.begin(), ip_bin.end());
                            }
                        }
                    }
                    pm.peers.push_back(std::move(pi));
                }
            }
        }
        parsed.peers = std::move(pm);
    } else {
        parsed.type = MessageType::Unknown;
    }

    return parsed;
}

}  // namespace ldns::net
