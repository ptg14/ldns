#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "core/util/result.hpp"

namespace ldns::crypto {

// Initializes libsodium. Safe to call multiple times.
Result<void> init();

// Computes SHA-256 digest (32 bytes).
std::array<std::uint8_t, 32> sha256(std::span<const std::uint8_t> data);

// Computes 8-byte network hash: SHA-256("ldns-net-v1:" || network_name)[0:8]
// per protocol.md §2.2.
std::array<std::uint8_t, 8> net_hash(std::string_view network_name);

// Fills buffer with CSPRNG random bytes.
void random_bytes(std::span<std::uint8_t> out);

// Generates 32 random bytes (e.g. for nonces).
std::array<std::uint8_t, 32> random_bytes_32();

}  // namespace ldns::crypto
