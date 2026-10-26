#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/util/id.hpp"
#include "core/util/result.hpp"

namespace ldns::net {

constexpr std::size_t kMaxFrameSize = 4 * 1024 * 1024; // 4 MiB (protocol.md §4.1)
constexpr std::size_t kMaxPeersLimit = 64;              // protocol.md §10

enum class ProtocolErrorCode : std::uint8_t {
    Version = 1,
    Network = 2,
    Auth = 3,
    Malformed = 4,
    Busy = 5,
    TooManyPeers = 6,
    Self = 7,
    Duplicate = 8,
    NotAllowed = 9,
};

std::string_view error_code_name(ProtocolErrorCode code);

struct HelloMessage {
    std::array<std::uint8_t, 2> version{1, 0};
    NodeId node_id{};
    std::string name;
    std::array<std::uint8_t, 8> net{};
    std::uint64_t epoch{0};
    std::uint64_t seq{0};
    std::vector<std::vector<std::uint8_t>> ips;
    std::uint16_t sync_port{5391};
    std::uint16_t dns_port{53};
    std::vector<std::string> caps{"relay", "snapshot-chunk"};
    std::array<std::uint8_t, 32> nonce{};
    bool psk{false};
};

struct PeerItem {
    NodeId node_id{};
    std::vector<std::vector<std::uint8_t>> ips;
    std::uint16_t sync_port{5391};
};

struct PeersMessage {
    std::vector<PeerItem> peers;
};

struct ErrorMessage {
    ProtocolErrorCode code{ProtocolErrorCode::Malformed};
    std::string msg;
};

struct Op {
    std::string op; // "put" | "del"
    std::string name;
    std::uint16_t type{1};
    std::uint32_t ttl{60};
    std::vector<std::vector<std::uint8_t>> data;
    std::uint64_t created{0};
    std::uint64_t ts{0};

    bool operator==(const Op& other) const = default;
};

struct SyncReqOwner {
    NodeId owner{};
    std::uint64_t epoch{0};
    std::uint64_t since_seq{0};

    bool operator==(const SyncReqOwner& other) const = default;
};

struct SyncReqMessage {
    std::uint32_t id{0};
    std::vector<SyncReqOwner> owners;
};

struct SyncDataMessage {
    std::uint32_t id{0};
    NodeId owner{};
    std::uint64_t epoch{0};
    std::uint64_t from_seq{0};
    std::uint64_t to_seq{0};
    bool snapshot{false};
    std::uint32_t chunk{0};
    bool last{true};
    std::vector<Op> ops;
};

struct SyncEndMessage {
    std::uint32_t id{0};
};

struct UpdateMessage {
    NodeId owner{};
    std::uint64_t epoch{0};
    std::uint64_t seq{0};
    std::vector<Op> ops;
};

enum class MessageType {
    Hello,
    Ping,
    Pong,
    Peers,
    SyncReq,
    SyncData,
    SyncEnd,
    Update,
    Bye,
    Error,
    Unknown,
};

struct ParsedMessage {
    MessageType type{MessageType::Unknown};
    std::string type_name;
    std::optional<HelloMessage> hello;
    std::optional<PeersMessage> peers;
    std::optional<ErrorMessage> error;
    std::optional<SyncReqMessage> sync_req;
    std::optional<SyncDataMessage> sync_data;
    std::optional<SyncEndMessage> sync_end;
    std::optional<UpdateMessage> update;
};

// Frames payload with 4-byte big-endian length prefix (protocol §4.1)
Result<std::vector<std::uint8_t>> frame_payload(std::span<const std::uint8_t> payload);

// Message encoders
Result<std::vector<std::uint8_t>> encode_hello_frame(const HelloMessage& msg);
Result<std::vector<std::uint8_t>> encode_ping_frame();
Result<std::vector<std::uint8_t>> encode_pong_frame();
Result<std::vector<std::uint8_t>> encode_bye_frame();
Result<std::vector<std::uint8_t>> encode_peers_frame(const PeersMessage& msg);
Result<std::vector<std::uint8_t>> encode_error_frame(const ErrorMessage& msg);
Result<std::vector<std::uint8_t>> encode_sync_req_frame(const SyncReqMessage& msg);
Result<std::vector<std::uint8_t>> encode_sync_data_frame(const SyncDataMessage& msg);
Result<std::vector<std::uint8_t>> encode_sync_end_frame(const SyncEndMessage& msg);
Result<std::vector<std::uint8_t>> encode_update_frame(const UpdateMessage& msg);

// Message parser from raw unencrypted payload
Result<ParsedMessage> parse_payload(std::span<const std::uint8_t> payload);

}  // namespace ldns::net
