#include "core/dns/message.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <unordered_map>

namespace ldns::dns {
namespace {

std::uint16_t read_u16(std::span<const std::uint8_t> data, std::size_t offset) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[offset]) << 8) |
                                      data[offset + 1]);
}

std::uint32_t read_u32(std::span<const std::uint8_t> data, std::size_t offset) noexcept {
    return (static_cast<std::uint32_t>(data[offset]) << 24) |
           (static_cast<std::uint32_t>(data[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(data[offset + 2]) << 8) |
           static_cast<std::uint32_t>(data[offset + 3]);
}

void append_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void append_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

Result<void> parse_rr(std::span<const std::uint8_t> packet, std::size_t& offset,
                      ResourceRecord& out) {
    auto name = Name::from_wire(packet, offset);
    if (!name) return name.error();

    if (offset + 10 > packet.size()) {
        return Error{102, "truncated resource record"};
    }
    const std::uint16_t type = read_u16(packet, offset);
    const std::uint16_t rr_class = read_u16(packet, offset + 2);
    const std::uint32_t ttl = read_u32(packet, offset + 4);
    const std::uint16_t rdlength = read_u16(packet, offset + 8);
    offset += 10;

    if (offset + rdlength > packet.size()) {
        return Error{103, "rdata runs past end of packet"};
    }

    std::vector<std::uint8_t> rdata(packet.begin() + static_cast<std::ptrdiff_t>(offset),
                                    packet.begin() + static_cast<std::ptrdiff_t>(offset + rdlength));

    // Decompress names embedded in the RDATA of types we understand.
    const auto wire_type = static_cast<RrType>(type);
    if (wire_type == RrType::CNAME || wire_type == RrType::NS || wire_type == RrType::PTR) {
        std::size_t name_offset = offset;
        auto embedded = Name::from_wire(packet, name_offset);
        if (!embedded) return embedded.error();
        if (name_offset != offset + rdlength) {
            return Error{104, "embedded name does not fill rdata"};
        }
        const auto wire = embedded.value().wire();
        rdata.assign(wire.begin(), wire.end());
    } else if (wire_type == RrType::SOA) {
        std::size_t cursor = offset;
        auto primary = Name::from_wire(packet, cursor);
        if (!primary) return primary.error();
        auto mail = Name::from_wire(packet, cursor);
        if (!mail) return mail.error();
        if (cursor + 20 != offset + rdlength) {
            return Error{105, "malformed SOA rdata"};
        }
        rdata.clear();
        const auto primary_wire = primary.value().wire();
        const auto mail_wire = mail.value().wire();
        rdata.insert(rdata.end(), primary_wire.begin(), primary_wire.end());
        rdata.insert(rdata.end(), mail_wire.begin(), mail_wire.end());
        rdata.insert(rdata.end(), packet.begin() + static_cast<std::ptrdiff_t>(cursor),
                     packet.begin() + static_cast<std::ptrdiff_t>(cursor + 20));
    } else if (wire_type == RrType::SRV) {
        if (rdlength < 7) return Error{106, "malformed SRV rdata"};
        std::size_t cursor = offset + 6;
        auto target = Name::from_wire(packet, cursor);
        if (!target) return target.error();
        if (cursor != offset + rdlength) {
            return Error{106, "malformed SRV rdata"};
        }
        const auto target_wire = target.value().wire();
        rdata.clear();
        rdata.insert(rdata.end(), packet.begin() + static_cast<std::ptrdiff_t>(offset),
                     packet.begin() + static_cast<std::ptrdiff_t>(offset + 6));
        rdata.insert(rdata.end(), target_wire.begin(), target_wire.end());
    }

    offset += rdlength;
    out.name = std::move(name.value());
    out.type = type;
    out.rr_class = rr_class;
    out.ttl = ttl;
    out.rdata = std::move(rdata);
    return {};
}

Result<void> parse_section(std::span<const std::uint8_t> packet, std::size_t& offset,
                           std::uint16_t count, std::vector<ResourceRecord>& out) {
    out.clear();
    out.reserve(std::min<std::size_t>(count, 64));
    for (std::uint16_t i = 0; i < count; ++i) {
        ResourceRecord rr;
        auto status = parse_rr(packet, offset, rr);
        if (!status) return status.error();
        out.push_back(std::move(rr));
    }
    return {};
}

// Compression-aware name writer. `names` maps a wire suffix -> offset in `out`.
void write_name(std::vector<std::uint8_t>& out, std::span<const std::uint8_t> wire,
                std::unordered_map<std::string, std::uint16_t>& names) {
    std::size_t pos = 0;
    while (wire[pos] != 0) {
        const std::string suffix(reinterpret_cast<const char*>(wire.data() + pos),
                                 wire.size() - pos);
        const auto found = names.find(suffix);
        if (found != names.end()) {
            const std::uint16_t pointer = static_cast<std::uint16_t>(0xC000 | found->second);
            out.push_back(static_cast<std::uint8_t>(pointer >> 8));
            out.push_back(static_cast<std::uint8_t>(pointer & 0xFF));
            return;
        }
        if (out.size() < 0x3FFF) {
            names.emplace(suffix, static_cast<std::uint16_t>(out.size()));
        }
        const std::size_t len = wire[pos];
        out.insert(out.end(), wire.begin() + static_cast<std::ptrdiff_t>(pos),
                   wire.begin() + static_cast<std::ptrdiff_t>(pos + 1 + len));
        pos += 1 + len;
    }
    out.push_back(0);
}


// RDATA embeds names for these types; they are compressed when written, so we
// re-encode from the decompressed form rather than copying bytes verbatim.
void write_rdata(std::vector<std::uint8_t>& out, const ResourceRecord& rr,
                 std::unordered_map<std::string, std::uint16_t>& names) {
    const auto wire_type = static_cast<RrType>(rr.type);
    if (wire_type == RrType::CNAME || wire_type == RrType::NS || wire_type == RrType::PTR) {
        std::size_t offset = 0;
        auto name = Name::from_wire(rr.rdata, offset);
        if (name && offset == rr.rdata.size()) {
            write_name(out, name.value().wire(), names);
            return;
        }
    } else if (wire_type == RrType::SOA) {
        std::size_t offset = 0;
        auto primary = Name::from_wire(rr.rdata, offset);
        if (primary) {
            auto mail = Name::from_wire(rr.rdata, offset);
            if (mail && offset + 20 == rr.rdata.size()) {
                write_name(out, primary.value().wire(), names);
                write_name(out, mail.value().wire(), names);
                out.insert(out.end(), rr.rdata.begin() + static_cast<std::ptrdiff_t>(offset),
                           rr.rdata.end());
                return;
            }
        }
    }
    out.insert(out.end(), rr.rdata.begin(), rr.rdata.end());
}

void write_rr(std::vector<std::uint8_t>& out, const ResourceRecord& rr,
              std::unordered_map<std::string, std::uint16_t>& names) {
    write_name(out, rr.name.wire(), names);
    append_u16(out, rr.type);
    append_u16(out, rr.rr_class);
    append_u32(out, rr.ttl);

    const std::size_t length_at = out.size();
    append_u16(out, 0);
    write_rdata(out, rr, names);
    const auto length = static_cast<std::uint16_t>(out.size() - length_at - 2);
    out[length_at] = static_cast<std::uint8_t>(length >> 8);
    out[length_at + 1] = static_cast<std::uint8_t>(length & 0xFF);
}

}  // namespace

// ---------------------------------------------------------------------------
// Message
// ---------------------------------------------------------------------------

Result<Message> Message::parse(std::span<const std::uint8_t> packet) {
    if (packet.size() < kHeaderSize) {
        return Error{100, "packet shorter than DNS header"};
    }

    Message msg;
    msg.id = read_u16(packet, 0);
    msg.flags = read_u16(packet, 2);
    const std::uint16_t qdcount = read_u16(packet, 4);
    const std::uint16_t ancount = read_u16(packet, 6);
    const std::uint16_t nscount = read_u16(packet, 8);
    const std::uint16_t arcount = read_u16(packet, 10);

    if (qdcount > kMaxRecords || ancount > kMaxRecords || nscount > kMaxRecords ||
        arcount > kMaxRecords) {
        return Error{101, "record count exceeds limit"};
    }

    std::size_t offset = kHeaderSize;
    msg.questions.reserve(std::min<std::size_t>(qdcount, 64));
    for (std::uint16_t i = 0; i < qdcount; ++i) {
        auto name = Name::from_wire(packet, offset);
        if (!name) return name.error();
        if (offset + 4 > packet.size()) {
            return Error{102, "truncated question"};
        }
        Question q;
        q.name = std::move(name.value());
        q.type = read_u16(packet, offset);
        q.qclass = read_u16(packet, offset + 2);
        offset += 4;
        msg.questions.push_back(std::move(q));
    }

    if (auto status = parse_section(packet, offset, ancount, msg.answers); !status) {
        return status.error();
    }
    if (auto status = parse_section(packet, offset, nscount, msg.authority); !status) {
        return status.error();
    }
    if (auto status = parse_section(packet, offset, arcount, msg.additional); !status) {
        return status.error();
    }

    // Pull out EDNS(0) from the additional section (SPEC §8, RFC 6891).
    // At most one OPT record is allowed, and it must be in additional.
    const auto opt_it = std::find_if(msg.additional.begin(), msg.additional.end(),
                                     [](const ResourceRecord& rr) { return rr.is_opt(); });
    if (opt_it != msg.additional.end()) {
        if (std::find_if(opt_it + 1, msg.additional.end(),
                         [](const ResourceRecord& rr) { return rr.is_opt(); }) !=
            msg.additional.end()) {
            return Error{108, "multiple EDNS OPT records"};
        }
        EdnsInfo edns;
        // RFC 6891 §6.2.3: values below 512 are treated as 512.
        edns.udp_payload = std::max<std::uint16_t>(opt_it->rr_class, 512);
        edns.extended_rcode = static_cast<std::uint8_t>(opt_it->ttl >> 24);
        edns.version = static_cast<std::uint8_t>((opt_it->ttl >> 16) & 0xFF);
        edns.do_bit = (opt_it->ttl & kEdnsDoBit) != 0;
        msg.edns = edns;
        msg.additional.erase(opt_it);
    }

    return msg;
}

Result<std::vector<std::uint8_t>> Message::encode(std::size_t max_size) const {
    // Section counts are 16-bit on the wire. Truncating with static_cast would
    // emit a header that disagrees with the body, producing a packet no parser
    // can read. Refuse instead of corrupting.
    std::size_t extra = 0;
    for (const auto& rr : additional) {
        if (!rr.is_opt()) ++extra;
    }
    if (edns) ++extra;
    if (questions.size() > 0xFFFF || answers.size() > 0xFFFF ||
        authority.size() > 0xFFFF || extra > 0xFFFF) {
        return Error{101, "record count exceeds the 16-bit DNS wire limit"};
    }

    std::vector<std::uint8_t> out;
    out.reserve(std::min<std::size_t>(max_size, 4096));

    append_u16(out, id);
    append_u16(out, flags);
    // EDNS is represented by `edns`; OPT entries in `additional` are ignored
    // so encoder output can never carry two OPTs (RFC 6891 §6.1.1).
    std::size_t additional_count = 0;
    for (const auto& rr : additional) {
        if (!rr.is_opt()) ++additional_count;
    }
    if (edns) ++additional_count;

    append_u16(out, static_cast<std::uint16_t>(questions.size()));
    append_u16(out, static_cast<std::uint16_t>(answers.size()));
    append_u16(out, static_cast<std::uint16_t>(authority.size()));
    append_u16(out, static_cast<std::uint16_t>(additional_count));

    std::unordered_map<std::string, std::uint16_t> names;
    for (const auto& q : questions) {
        write_name(out, q.name.wire(), names);
        append_u16(out, q.type);
        append_u16(out, q.qclass);
    }
    for (const auto& rr : answers) write_rr(out, rr, names);
    for (const auto& rr : authority) write_rr(out, rr, names);
    for (const auto& rr : additional) {
        // EDNS lives in `edns`; an OPT left in `additional` would produce a
        // second OPT on the wire (RFC 6891 allows at most one).
        if (rr.is_opt()) continue;
        write_rr(out, rr, names);
    }

    if (edns) {
        out.push_back(0);  // root owner name
        append_u16(out, static_cast<std::uint16_t>(RrType::OPT));
        append_u16(out, edns->udp_payload);
        std::uint32_t ttl = (static_cast<std::uint32_t>(edns->extended_rcode) << 24) |
                            (static_cast<std::uint32_t>(edns->version) << 16);
        if (edns->do_bit) ttl |= kEdnsDoBit;
        append_u32(out, ttl);
        append_u16(out, 0);  // rdlength
    }

    if (out.size() > max_size || out.size() > 65535) {
        return Error{1, "encoded message exceeds transport limit"};
    }
    return out;
}

std::vector<std::uint8_t> Message::encode_truncated() const {
    Message truncated;
    truncated.id = id;
    truncated.flags = static_cast<std::uint16_t>(flags | kTc);
    truncated.questions = questions;
    auto encoded = truncated.encode(65535);
    return encoded ? encoded.value() : std::vector<std::uint8_t>{};
}

// ---------------------------------------------------------------------------
// RDATA helpers
// ---------------------------------------------------------------------------

Result<std::vector<std::uint8_t>> encode_rdata_a(const std::uint8_t* ipv4) {
    if (!ipv4) return Error{110, "null IPv4 pointer"};
    return std::vector<std::uint8_t>(ipv4, ipv4 + 4);
}

Result<std::vector<std::uint8_t>> encode_rdata_aaaa(const std::uint8_t* ipv6) {
    if (!ipv6) return Error{110, "null IPv6 pointer"};
    return std::vector<std::uint8_t>(ipv6, ipv6 + 16);
}

Result<std::vector<std::uint8_t>> encode_rdata_name(const Name& target) {
    const auto wire = target.wire();
    return std::vector<std::uint8_t>(wire.begin(), wire.end());
}

Result<std::vector<std::uint8_t>> encode_rdata_txt(std::string_view text) {
    std::vector<std::uint8_t> out;
    if (text.empty()) {
        out.push_back(0);
        return out;
    }
    std::size_t offset = 0;
    while (offset < text.size()) {
        const std::size_t chunk = std::min<std::size_t>(text.size() - offset, 255);
        out.push_back(static_cast<std::uint8_t>(chunk));
        out.insert(out.end(), text.begin() + static_cast<std::ptrdiff_t>(offset),
                   text.begin() + static_cast<std::ptrdiff_t>(offset + chunk));
        offset += chunk;
    }
    return out;
}

Result<std::vector<std::uint8_t>> encode_rdata_soa(const Name& primary, const Name& mail,
                                                   std::uint32_t serial, std::uint32_t refresh,
                                                   std::uint32_t retry, std::uint32_t expire,
                                                   std::uint32_t minimum) {
    std::vector<std::uint8_t> out;
    const auto primary_wire = primary.wire();
    const auto mail_wire = mail.wire();
    out.insert(out.end(), primary_wire.begin(), primary_wire.end());
    out.insert(out.end(), mail_wire.begin(), mail_wire.end());
    append_u32(out, serial);
    append_u32(out, refresh);
    append_u32(out, retry);
    append_u32(out, expire);
    append_u32(out, minimum);
    return out;
}

Result<std::vector<std::uint8_t>> encode_rdata_srv(std::uint16_t priority, std::uint16_t weight,
                                                   std::uint16_t port, const Name& target) {
    std::vector<std::uint8_t> out;
    append_u16(out, priority);
    append_u16(out, weight);
    append_u16(out, port);
    const auto wire = target.wire();
    out.insert(out.end(), wire.begin(), wire.end());
    return out;
}

Result<Name> decode_rdata_name(std::span<const std::uint8_t> rdata) {
    std::size_t offset = 0;
    auto name = Name::from_wire(rdata, offset);
    if (!name) return name.error();
    if (offset != rdata.size()) return Error{111, "trailing bytes after name in rdata"};
    return name;
}

Result<std::string> decode_rdata_txt(std::span<const std::uint8_t> rdata) {
    std::string out;
    std::size_t offset = 0;
    while (offset < rdata.size()) {
        const std::size_t len = rdata[offset];
        if (offset + 1 + len > rdata.size()) return Error{112, "malformed TXT rdata"};
        out.append(reinterpret_cast<const char*>(rdata.data() + offset + 1), len);
        offset += 1 + len;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Address formatting
// ---------------------------------------------------------------------------

Result<std::vector<std::uint8_t>> parse_ipv4(std::string_view text) {
    std::vector<std::uint8_t> out;
    std::size_t pos = 0;
    int parts = 0;
    while (pos <= text.size()) {
        const std::size_t dot = text.find('.', pos);
        const std::size_t end = dot == std::string_view::npos ? text.size() : dot;
        const std::string_view part = text.substr(pos, end - pos);
        if (part.empty() || part.size() > 3) return Error{120, "invalid IPv4 address"};
        int value = 0;
        for (const char c : part) {
            if (c < '0' || c > '9') return Error{120, "invalid IPv4 address"};
            value = value * 10 + (c - '0');
        }
        if (value > 255) return Error{120, "invalid IPv4 address"};
        out.push_back(static_cast<std::uint8_t>(value));
        ++parts;
        if (dot == std::string_view::npos) break;
        pos = dot + 1;
    }
    if (parts != 4) return Error{120, "invalid IPv4 address"};
    return out;
}

std::string format_ipv4(std::span<const std::uint8_t> bytes) {
    if (bytes.size() != 4) return {};
    return std::to_string(bytes[0]) + "." + std::to_string(bytes[1]) + "." +
           std::to_string(bytes[2]) + "." + std::to_string(bytes[3]);
}

Result<std::vector<std::uint8_t>> parse_ipv6(std::string_view text) {
    // Accepts the full RFC 4291 syntax, including one "::" and an embedded
    // dotted-quad tail. Implemented manually to keep the dependency surface
    // small and the failure mode predictable.
    std::array<std::uint8_t, 16> bytes{};
    std::size_t pos = 0;
    int group = 0;
    int gap = -1;

    if (text.empty()) return Error{121, "invalid IPv6 address"};
    if (text.size() >= 2 && text[0] == ':' && text[1] == ':') {
        gap = 0;
        pos = 2;
    } else if (text[0] == ':') {
        return Error{121, "invalid IPv6 address"};
    }

    while (pos < text.size()) {
        if (group >= 8) {
            if (gap < 0 && text.substr(pos - 1) != "::") return Error{121, "invalid IPv6 address"};
            return Error{121, "invalid IPv6 address"};
        }

        const std::size_t colon = text.find(':', pos);
        const std::size_t end = colon == std::string_view::npos ? text.size() : colon;
        const std::string_view part = text.substr(pos, end - pos);

        if (part.empty()) {
            if (gap >= 0 || end != text.size() || colon == std::string_view::npos) {
                return Error{121, "invalid IPv6 address"};
            }
            gap = group;
            break;
        }

        if (part.find('.') != std::string_view::npos) {
            // Embedded IPv4 occupies two groups.
            auto v4 = parse_ipv4(part);
            if (!v4) return Error{121, "invalid IPv4 tail in IPv6 address"};
            if (group > 6) return Error{121, "invalid IPv6 address"};
            bytes[group * 2] = v4.value()[0];
            bytes[group * 2 + 1] = v4.value()[1];
            bytes[group * 2 + 2] = v4.value()[2];
            bytes[group * 2 + 3] = v4.value()[3];
            group += 2;
            break;
        }

        if (part.size() > 4) return Error{121, "invalid IPv6 address"};
        std::uint16_t value = 0;
        for (const char c : part) {
            int digit = -1;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
            else return Error{121, "invalid IPv6 address"};
            value = static_cast<std::uint16_t>((value << 4) | static_cast<std::uint16_t>(digit));
        }
        bytes[group * 2] = static_cast<std::uint8_t>(value >> 8);
        bytes[group * 2 + 1] = static_cast<std::uint8_t>(value & 0xFF);
        ++group;

        if (colon == std::string_view::npos) break;
        pos = colon + 1;
        if (pos == text.size()) return Error{121, "invalid IPv6 address"};
        if (text[pos] == ':') {
            if (gap >= 0) return Error{121, "invalid IPv6 address"};
            gap = group;
            pos += 1;
            if (pos == text.size()) break;
        }
    }

    if (group < 8) {
        if (gap < 0) return Error{121, "invalid IPv6 address"};
        const int shift = 8 - group;
        if (shift <= 0) return Error{121, "invalid IPv6 address"};
        for (int i = group - 1; i >= gap; --i) {
            bytes[(i + shift) * 2] = bytes[i * 2];
            bytes[(i + shift) * 2 + 1] = bytes[i * 2 + 1];
        }
        for (int i = gap; i < gap + shift; ++i) {
            bytes[i * 2] = 0;
            bytes[i * 2 + 1] = 0;
        }
    } else if (gap >= 0 && group != 8) {
        return Error{121, "invalid IPv6 address"};
    }

    return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
}

std::string format_ipv6(std::span<const std::uint8_t> bytes) {
    if (bytes.size() != 16) return {};
    std::array<std::uint16_t, 8> groups{};
    for (std::size_t i = 0; i < 8; ++i) {
        groups[i] = static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[i * 2]) << 8) |
                                               bytes[i * 2 + 1]);
    }

    // Longest run of zero groups (length >= 2) for "::" compression.
    std::size_t best_start = 8;
    std::size_t best_len = 0;
    std::size_t run_start = 0;
    std::size_t run_len = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        if (groups[i] == 0) {
            if (run_len == 0) run_start = i;
            ++run_len;
            if (run_len > best_len) {
                best_len = run_len;
                best_start = run_start;
            }
        } else {
            run_len = 0;
        }
    }

    std::string out;
    char buffer[8];
    for (std::size_t i = 0; i < 8; ++i) {
        if (best_len >= 2 && i == best_start) {
            out += "::";
            i += best_len - 1;
            continue;
        }
        if (!out.empty() && out.back() != ':') out.push_back(':');
        std::snprintf(buffer, sizeof(buffer), "%x", groups[i]);
        out += buffer;
    }
    return out;
}

std::string_view rr_type_name(std::uint16_t type) {
    switch (static_cast<RrType>(type)) {
        case RrType::A: return "A";
        case RrType::NS: return "NS";
        case RrType::CNAME: return "CNAME";
        case RrType::SOA: return "SOA";
        case RrType::PTR: return "PTR";
        case RrType::TXT: return "TXT";
        case RrType::AAAA: return "AAAA";
        case RrType::SRV: return "SRV";
        case RrType::OPT: return "OPT";
        case RrType::ANY: return "ANY";
    }
    return "TYPE?";
}

Result<std::uint16_t> rr_type_from_name(std::string_view name) {
    if (name == "A") return static_cast<std::uint16_t>(RrType::A);
    if (name == "NS") return static_cast<std::uint16_t>(RrType::NS);
    if (name == "CNAME") return static_cast<std::uint16_t>(RrType::CNAME);
    if (name == "SOA") return static_cast<std::uint16_t>(RrType::SOA);
    if (name == "PTR") return static_cast<std::uint16_t>(RrType::PTR);
    if (name == "TXT") return static_cast<std::uint16_t>(RrType::TXT);
    if (name == "AAAA") return static_cast<std::uint16_t>(RrType::AAAA);
    if (name == "SRV") return static_cast<std::uint16_t>(RrType::SRV);
    if (name == "ANY") return static_cast<std::uint16_t>(RrType::ANY);
    return Error{130, "unknown RR type"};
}

std::string_view rcode_name(Rcode code) {
    switch (code) {
        case Rcode::NoError: return "NOERROR";
        case Rcode::FormErr: return "FORMERR";
        case Rcode::ServFail: return "SERVFAIL";
        case Rcode::NxDomain: return "NXDOMAIN";
        case Rcode::NotImp: return "NOTIMP";
        case Rcode::Refused: return "REFUSED";
    }
    return "UNKNOWN";
}

}  // namespace ldns::dns
