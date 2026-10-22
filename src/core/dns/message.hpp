#pragma once

// DNS wire codec (SPEC §8, PLAN M1).
//
// Strict parser: every read is bounds-checked, compression pointers are depth
// limited and must point backwards, and record counts are capped. The encoder
// emits names with RFC 1035 compression. Nothing here allocates unbounded
// memory based on attacker-controlled counts.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/dns/name.hpp"
#include "core/util/result.hpp"

namespace ldns::dns {

// Record types supported in v1 (SPEC §5.2, §8); others are kept as opaque data.
enum class RrType : std::uint16_t {
    A = 1,
    NS = 2,
    CNAME = 5,
    SOA = 6,
    PTR = 12,
    TXT = 16,
    AAAA = 28,
    SRV = 33,
    OPT = 41,
    ANY = 255,
};

enum class Rcode : std::uint8_t {
    NoError = 0,
    FormErr = 1,
    ServFail = 2,
    NxDomain = 3,
    NotImp = 4,
    Refused = 5,
};

inline constexpr std::uint16_t kClassIn = 1;

// Header flag bits (RFC 1035 §4.1.1).
enum HeaderFlags : std::uint16_t {
    kQr = 0x8000,
    kAa = 0x0400,
    kTc = 0x0200,
    kRd = 0x0100,
    kRa = 0x0080,
    kOpcodeQuery = 0x0000,
    kOpcodeMask = 0x7800,
    kRcodeMask = 0x000F,
};

inline constexpr std::size_t kHeaderSize = 12;
inline constexpr std::size_t kMaxRecords = 4096;
inline constexpr std::uint16_t kDefaultUdpPayload = 512;
inline constexpr std::uint16_t kEdnsUdpPayload = 1232;
inline constexpr std::uint8_t kEdnsVersion = 0;
inline constexpr std::uint16_t kEdnsDoBit = 0x8000;

struct Question {
    Name name;
    std::uint16_t type{};
    std::uint16_t qclass{kClassIn};
};

// Resource record. `rdata` is the decompressed RDATA; for CNAME/NS/SOA the
// embedded names are stored uncompressed.
struct ResourceRecord {
    Name name;
    std::uint16_t type{};
    std::uint16_t rr_class{kClassIn};
    std::uint32_t ttl{};
    std::vector<std::uint8_t> rdata;

    bool is_opt() const noexcept { return type == static_cast<std::uint16_t>(RrType::OPT); }
};

struct EdnsInfo {
    std::uint16_t udp_payload{kEdnsUdpPayload};
    std::uint8_t version{kEdnsVersion};
    bool do_bit{};
    std::uint8_t extended_rcode{};
};

struct Message {
    std::uint16_t id{};
    std::uint16_t flags{};
    std::vector<Question> questions;
    std::vector<ResourceRecord> answers;
    std::vector<ResourceRecord> authority;
    std::vector<ResourceRecord> additional;

    std::optional<EdnsInfo> edns;

    // --- header helpers -----------------------------------------------------
    bool is_response() const noexcept { return (flags & kQr) != 0; }
    std::uint8_t rcode() const noexcept { return static_cast<std::uint8_t>(flags & kRcodeMask); }
    std::uint16_t opcode() const noexcept { return (flags & kOpcodeMask) >> 11; }
    bool truncated() const noexcept { return (flags & kTc) != 0; }
    void set_flag(std::uint16_t flag, bool on) noexcept {
        flags = on ? static_cast<std::uint16_t>(flags | flag)
                   : static_cast<std::uint16_t>(flags & ~flag);
    }
    void set_rcode(Rcode code) noexcept {
        flags = static_cast<std::uint16_t>((flags & ~kRcodeMask) | static_cast<std::uint16_t>(code));
    }

    // --- codec --------------------------------------------------------------
    static Result<Message> parse(std::span<const std::uint8_t> packet);

    // Serializes with name compression. `max_size` is the transport limit; if
    // the encoded message would exceed it, returns Error{code=1} (caller sets
    // TC and sends a truncated response).
    Result<std::vector<std::uint8_t>> encode(std::size_t max_size = 65535) const;

    // Serializes with answers/authority/additional dropped and TC set.
    std::vector<std::uint8_t> encode_truncated() const;
};

// --- RDATA builders / decoders ---------------------------------------------

Result<std::vector<std::uint8_t>> encode_rdata_a(const std::uint8_t* ipv4);
Result<std::vector<std::uint8_t>> encode_rdata_aaaa(const std::uint8_t* ipv6);
Result<std::vector<std::uint8_t>> encode_rdata_name(const Name& target);
Result<std::vector<std::uint8_t>> encode_rdata_txt(std::string_view text);
Result<std::vector<std::uint8_t>> encode_rdata_soa(const Name& primary, const Name& mail,
                                                   std::uint32_t serial, std::uint32_t refresh,
                                                   std::uint32_t retry, std::uint32_t expire,
                                                   std::uint32_t minimum);
Result<std::vector<std::uint8_t>> encode_rdata_srv(std::uint16_t priority, std::uint16_t weight,
                                                   std::uint16_t port, const Name& target);

Result<Name> decode_rdata_name(std::span<const std::uint8_t> rdata);
Result<std::string> decode_rdata_txt(std::span<const std::uint8_t> rdata);

// Convenience: IPv4 dotted-quad <-> 4 bytes.
Result<std::vector<std::uint8_t>> parse_ipv4(std::string_view text);
std::string format_ipv4(std::span<const std::uint8_t> bytes);
Result<std::vector<std::uint8_t>> parse_ipv6(std::string_view text);
std::string format_ipv6(std::span<const std::uint8_t> bytes);

std::string_view rr_type_name(std::uint16_t type);
Result<std::uint16_t> rr_type_from_name(std::string_view name);

}  // namespace ldns::dns
