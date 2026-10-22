#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace ldns {

// 128-bit node identifier (SPEC §2). Ordering is big-endian lexicographic,
// which matches std::array comparison (see protocol.md §2.2).
using NodeId = std::array<std::uint8_t, 16>;

// Generates a random NodeId. Backed by the OS CSPRNG (std::random_device);
// replaced by libsodium in M9 when PSK support lands.
NodeId generate_node_id();

// Lowercase hex, no separators (32 chars).
std::string to_hex(const NodeId& id);
std::string to_hex(std::span<const std::uint8_t> bytes);

// Parses exactly 32 hex chars. Returns false on any malformed input.
bool from_hex(std::string_view hex, NodeId& out);

bool is_zero(const NodeId& id) noexcept;

// Diagnostic short form: first 4 hex chars + ellipsis, e.g. "a3f9…".
std::string short_id(const NodeId& id);

}  // namespace ldns
