#include "core/net/protocol_message.hpp"

#include <nlohmann/json.hpp>
#include <algorithm>
#include "core/dns/name.hpp"
#include "core/dns/message.hpp"

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


json encode_op(const Op& op) {
    json j;
    j["op"] = op.op;
    j["name"] = op.name;
    j["type"] = op.type;
    if (op.op == "put") {
        j["ttl"] = op.ttl;
        json data_arr = json::array();
        for (const auto& d : op.data) {
            if (op.type == 5) {
                std::size_t offset = 0;
                auto parsed_name = dns::Name::from_wire(d, offset);
                if (parsed_name) {
                    data_arr.push_back(parsed_name.value().to_string());
                } else {
                    data_arr.push_back(std::string(d.begin(), d.end()));
                }
            } else {
                data_arr.push_back(json::binary(d));
            }
        }
        j["data"] = std::move(data_arr);
        j["created"] = op.created;
    }
    j["ts"] = op.ts;
    return j;
}

Op decode_op(const json& j) {
    Op op;
    if (j.contains("op") && j["op"].is_string()) op.op = j["op"].get<std::string>();
    if (j.contains("name") && j["name"].is_string()) op.name = j["name"].get<std::string>();
    if (j.contains("type") && j["type"].is_number_unsigned()) op.type = j["type"].get<std::uint16_t>();
    if (j.contains("ts") && j["ts"].is_number_unsigned()) op.ts = j["ts"].get<std::uint64_t>();
    if (op.op == "put") {
        if (j.contains("ttl") && j["ttl"].is_number_unsigned()) op.ttl = j["ttl"].get<std::uint32_t>();
        if (j.contains("created") && j["created"].is_number_unsigned()) op.created = j["created"].get<std::uint64_t>();
        if (j.contains("data") && j["data"].is_array()) {
            for (const auto& item : j["data"]) {
                if (item.is_binary()) {
                    const auto& b = item.get_binary();
                    op.data.emplace_back(b.begin(), b.end());
                } else if (item.is_string()) {
                    std::string s = item.get<std::string>();
                    if (op.type == 5) {
                        auto n = dns::Name::from_presentation(s);
                        if (n) {
                            auto w = n.value().wire();
                            op.data.emplace_back(w.begin(), w.end());
                        } else {
                            op.data.emplace_back(s.begin(), s.end());
                        }
                    } else {
                        op.data.emplace_back(s.begin(), s.end());
                    }
                }
            }
        }
    }
    return op;
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

Result<std::vector<std::uint8_t>> encode_auth_frame(const AuthMessage& msg) {
    json j;
    j["t"] = "AUTH";
    std::vector<std::uint8_t> mac_bytes(msg.mac.begin(), msg.mac.end());
    j["mac"] = json::binary(std::move(mac_bytes));
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

Result<std::vector<std::uint8_t>> encode_sync_req_frame(const SyncReqMessage& msg) {
    json j;
    j["t"] = "SYNC_REQ";
    j["id"] = msg.id;
    json owners_arr = json::array();
    for (const auto& o : msg.owners) {
        json item;
        std::vector<std::uint8_t> node_bytes(o.owner.begin(), o.owner.end());
        item["owner"] = json::binary(std::move(node_bytes));
        item["epoch"] = o.epoch;
        item["since_seq"] = o.since_seq;
        owners_arr.push_back(std::move(item));
    }
    j["owners"] = std::move(owners_arr);

    std::vector<std::uint8_t> payload;
    json::to_msgpack(j, payload);
    return frame_payload(payload);
}

Result<std::vector<std::uint8_t>> encode_sync_data_frame(const SyncDataMessage& msg) {
    json j;
    j["t"] = "SYNC_DATA";
    j["id"] = msg.id;
    std::vector<std::uint8_t> node_bytes(msg.owner.begin(), msg.owner.end());
    j["owner"] = json::binary(std::move(node_bytes));
    j["epoch"] = msg.epoch;
    j["from_seq"] = msg.from_seq;
    j["to_seq"] = msg.to_seq;
    j["snapshot"] = msg.snapshot;
    j["chunk"] = msg.chunk;
    j["last"] = msg.last;

    json ops_arr = json::array();
    for (const auto& op : msg.ops) {
        ops_arr.push_back(encode_op(op));
    }
    j["ops"] = std::move(ops_arr);

    std::vector<std::uint8_t> payload;
    json::to_msgpack(j, payload);
    return frame_payload(payload);
}

Result<std::vector<std::uint8_t>> encode_sync_end_frame(const SyncEndMessage& msg) {
    json j;
    j["t"] = "SYNC_END";
    j["id"] = msg.id;

    std::vector<std::uint8_t> payload;
    json::to_msgpack(j, payload);
    return frame_payload(payload);
}

Result<std::vector<std::uint8_t>> encode_update_frame(const UpdateMessage& msg) {
    json j;
    j["t"] = "UPDATE";
    std::vector<std::uint8_t> node_bytes(msg.owner.begin(), msg.owner.end());
    j["owner"] = json::binary(std::move(node_bytes));
    j["epoch"] = msg.epoch;
    j["seq"] = msg.seq;

    json ops_arr = json::array();
    for (const auto& op : msg.ops) {
        ops_arr.push_back(encode_op(op));
    }
    j["ops"] = std::move(ops_arr);

    std::vector<std::uint8_t> payload;
    json::to_msgpack(j, payload);
    return frame_payload(payload);
}

Result<std::vector<std::uint8_t>> encode_resolve_frame(const ResolveMessage& msg) {
    json j;
    j["t"] = "RESOLVE";
    j["id"] = msg.id;
    j["name"] = msg.name;
    j["type"] = msg.type;
    std::vector<std::uint8_t> payload;
    json::to_msgpack(j, payload);
    return frame_payload(payload);
}

Result<std::vector<std::uint8_t>> encode_resolve_response_frame(const ResolveResponseMessage& msg) {
    json j;
    j["t"] = "RESOLVE_R";
    j["id"] = msg.id;
    j["rcode"] = msg.rcode;
    j["epoch"] = msg.epoch;
    j["seq"] = msg.seq;

    json answers_arr = json::array();
    for (const auto& a : msg.answers) {
        json item;
        item["name"] = a.name;
        item["type"] = a.type;
        item["ttl"] = a.ttl;
        json data_arr = json::array();
        for (const auto& d : a.data) {
            if (a.type == 5) {
                std::size_t offset = 0;
                auto parsed_name = dns::Name::from_wire(d, offset);
                if (parsed_name) {
                    data_arr.push_back(parsed_name.value().to_string());
                } else {
                    data_arr.push_back(std::string(d.begin(), d.end()));
                }
            } else {
                data_arr.push_back(json::binary(d));
            }
        }
        item["data"] = std::move(data_arr);
        answers_arr.push_back(std::move(item));
    }
    j["answers"] = std::move(answers_arr);

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
    } else if (parsed.type_name == "AUTH") {
        parsed.type = MessageType::Auth;
        AuthMessage a;
        if (j.contains("mac") && j["mac"].is_binary() && j["mac"].get_binary().size() == 32) {
            const auto& mac_bin = j["mac"].get_binary();
            std::copy_n(mac_bin.begin(), 32, a.mac.begin());
            parsed.auth = std::move(a);
        } else {
            return Error{823, "invalid or missing AUTH mac"};
        }
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
    } else if (parsed.type_name == "SYNC_REQ") {
        parsed.type = MessageType::SyncReq;
        SyncReqMessage sreq;
        if (j.contains("id") && j["id"].is_number_unsigned()) sreq.id = j["id"].get<std::uint32_t>();
        if (j.contains("owners") && j["owners"].is_array()) {
            for (const auto& item : j["owners"]) {
                if (item.is_object() && item.contains("owner") && item["owner"].is_binary() &&
                    item["owner"].get_binary().size() == 16) {
                    SyncReqOwner o;
                    const auto& obin = item["owner"].get_binary();
                    std::copy_n(obin.begin(), 16, o.owner.begin());
                    if (item.contains("epoch") && item["epoch"].is_number_unsigned()) o.epoch = item["epoch"].get<std::uint64_t>();
                    if (item.contains("since_seq") && item["since_seq"].is_number_unsigned()) o.since_seq = item["since_seq"].get<std::uint64_t>();
                    sreq.owners.push_back(std::move(o));
                }
            }
        }
        parsed.sync_req = std::move(sreq);
    } else if (parsed.type_name == "SYNC_DATA") {
        parsed.type = MessageType::SyncData;
        SyncDataMessage sdata;
        if (j.contains("id") && j["id"].is_number_unsigned()) sdata.id = j["id"].get<std::uint32_t>();
        if (j.contains("owner") && j["owner"].is_binary() && j["owner"].get_binary().size() == 16) {
            const auto& obin = j["owner"].get_binary();
            std::copy_n(obin.begin(), 16, sdata.owner.begin());
        }
        if (j.contains("epoch") && j["epoch"].is_number_unsigned()) sdata.epoch = j["epoch"].get<std::uint64_t>();
        if (j.contains("from_seq") && j["from_seq"].is_number_unsigned()) sdata.from_seq = j["from_seq"].get<std::uint64_t>();
        if (j.contains("to_seq") && j["to_seq"].is_number_unsigned()) sdata.to_seq = j["to_seq"].get<std::uint64_t>();
        if (j.contains("snapshot") && j["snapshot"].is_boolean()) sdata.snapshot = j["snapshot"].get<bool>();
        if (j.contains("chunk") && j["chunk"].is_number_unsigned()) sdata.chunk = j["chunk"].get<std::uint32_t>();
        if (j.contains("last") && j["last"].is_boolean()) sdata.last = j["last"].get<bool>();
        if (j.contains("ops") && j["ops"].is_array()) {
            for (const auto& op_item : j["ops"]) {
                if (op_item.is_object()) sdata.ops.push_back(decode_op(op_item));
            }
        }
        parsed.sync_data = std::move(sdata);
    } else if (parsed.type_name == "SYNC_END") {
        parsed.type = MessageType::SyncEnd;
        SyncEndMessage send;
        if (j.contains("id") && j["id"].is_number_unsigned()) send.id = j["id"].get<std::uint32_t>();
        parsed.sync_end = std::move(send);
    } else if (parsed.type_name == "UPDATE") {
        parsed.type = MessageType::Update;
        UpdateMessage upd;
        if (j.contains("owner") && j["owner"].is_binary() && j["owner"].get_binary().size() == 16) {
            const auto& obin = j["owner"].get_binary();
            std::copy_n(obin.begin(), 16, upd.owner.begin());
        }
        if (j.contains("epoch") && j["epoch"].is_number_unsigned()) upd.epoch = j["epoch"].get<std::uint64_t>();
        if (j.contains("seq") && j["seq"].is_number_unsigned()) upd.seq = j["seq"].get<std::uint64_t>();
        if (j.contains("ops") && j["ops"].is_array()) {
            for (const auto& op_item : j["ops"]) {
                if (op_item.is_object()) upd.ops.push_back(decode_op(op_item));
            }
        }
        parsed.update = std::move(upd);
    } else if (parsed.type_name == "RESOLVE") {
        parsed.type = MessageType::Resolve;
        ResolveMessage rmsg;
        if (j.contains("id") && j["id"].is_number_unsigned()) rmsg.id = j["id"].get<std::uint32_t>();
        if (j.contains("name") && j["name"].is_string()) rmsg.name = j["name"].get<std::string>();
        if (j.contains("type")) {
            if (j["type"].is_number_unsigned()) {
                rmsg.type = j["type"].get<std::uint16_t>();
            } else if (j["type"].is_string()) {
                auto parsed_type = dns::rr_type_from_name(j["type"].get<std::string>());
                if (parsed_type) rmsg.type = parsed_type.value();
            }
        }
        parsed.resolve = std::move(rmsg);
    } else if (parsed.type_name == "RESOLVE_R") {
        parsed.type = MessageType::ResolveResponse;
        ResolveResponseMessage rresp;
        if (j.contains("id") && j["id"].is_number_unsigned()) rresp.id = j["id"].get<std::uint32_t>();
        if (j.contains("rcode") && j["rcode"].is_number_unsigned()) rresp.rcode = j["rcode"].get<std::uint8_t>();
        if (j.contains("epoch") && j["epoch"].is_number_unsigned()) rresp.epoch = j["epoch"].get<std::uint64_t>();
        if (j.contains("seq") && j["seq"].is_number_unsigned()) rresp.seq = j["seq"].get<std::uint64_t>();
        if (j.contains("answers") && j["answers"].is_array()) {
            for (const auto& aitem : j["answers"]) {
                if (!aitem.is_object()) continue;
                ResolveAnswer ans;
                if (aitem.contains("name") && aitem["name"].is_string()) ans.name = aitem["name"].get<std::string>();
                if (aitem.contains("type")) {
                    if (aitem["type"].is_number_unsigned()) {
                        ans.type = aitem["type"].get<std::uint16_t>();
                    } else if (aitem["type"].is_string()) {
                        auto parsed_type = dns::rr_type_from_name(aitem["type"].get<std::string>());
                        if (parsed_type) ans.type = parsed_type.value();
                    }
                }
                if (aitem.contains("ttl") && aitem["ttl"].is_number_unsigned()) ans.ttl = aitem["ttl"].get<std::uint32_t>();
                if (aitem.contains("data") && aitem["data"].is_array()) {
                    for (const auto& dval : aitem["data"]) {
                        if (dval.is_binary()) {
                            const auto& b = dval.get_binary();
                            ans.data.emplace_back(b.begin(), b.end());
                        } else if (dval.is_string()) {
                            std::string s = dval.get<std::string>();
                            if (ans.type == 5) {
                                auto n = dns::Name::from_presentation(s);
                                if (n) {
                                    auto w = n.value().wire();
                                    ans.data.emplace_back(w.begin(), w.end());
                                } else {
                                    ans.data.emplace_back(s.begin(), s.end());
                                }
                            } else {
                                ans.data.emplace_back(s.begin(), s.end());
                            }
                        }
                    }
                }
                rresp.answers.push_back(std::move(ans));
            }
        }
        parsed.resolve_response = std::move(rresp);
    } else {
        parsed.type = MessageType::Unknown;
    }

    return parsed;
}

}  // namespace ldns::net
