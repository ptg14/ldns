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

enum class MessageType {
    Hello,
    Ping,
    Pong,
    Peers,
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

// Message parser from raw unencrypted payload
Result<ParsedMessage> parse_payload(std::span<const std::uint8_t> payload);

}  // namespace ldns::net
