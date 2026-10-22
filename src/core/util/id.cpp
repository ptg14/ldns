#include "core/util/id.hpp"

#include <algorithm>
#include <cctype>
#include <random>

namespace ldns {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

int hex_value(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

NodeId generate_node_id() {
    std::random_device rd;
    NodeId id{};
    for (std::size_t i = 0; i < id.size(); i += sizeof(unsigned int)) {
        const unsigned int value = rd();
        for (std::size_t b = 0; b < sizeof(unsigned int) && i + b < id.size(); ++b) {
            id[i + b] = static_cast<std::uint8_t>((value >> (8 * b)) & 0xFFu);
        }
    }
    return id;
}

std::string to_hex(std::span<const std::uint8_t> bytes) {
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        out.push_back(kHexDigits[byte >> 4]);
        out.push_back(kHexDigits[byte & 0x0F]);
    }
    return out;
}

std::string to_hex(const NodeId& id) {
    return to_hex(std::span<const std::uint8_t>(id.data(), id.size()));
}

bool from_hex(std::string_view hex, NodeId& out) {
    if (hex.size() != out.size() * 2) return false;
    NodeId parsed{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        const int hi = hex_value(hex[i * 2]);
        const int lo = hex_value(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        parsed[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    out = parsed;
    return true;
}

bool is_zero(const NodeId& id) noexcept {
    return std::all_of(id.begin(), id.end(), [](std::uint8_t b) { return b == 0; });
}

std::string short_id(const NodeId& id) {
    const std::string hex = to_hex(std::span<const std::uint8_t>(id.data(), 2));
    return hex + "\xE2\x80\xA6";  // U+2026 HORIZONTAL ELLIPSIS
}

}  // namespace ldns
