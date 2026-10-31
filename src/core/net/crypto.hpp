#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "core/util/result.hpp"

namespace ldns::crypto {

inline constexpr std::size_t kKeySize = 32;
inline constexpr std::size_t kAeadTagSize = 16;
inline constexpr std::size_t kAeadNonceSize = 24;
inline constexpr std::size_t kBeaconMacSize = 16;

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

// Computes HMAC-SHA256 (32 bytes).
std::array<std::uint8_t, 32> hmac_sha256(std::span<const std::uint8_t> key,
                                         std::span<const std::uint8_t> data);

// RFC 5869 HKDF-SHA256 Extract: PRK = HMAC-SHA256(salt, ikm).
std::array<std::uint8_t, 32> hkdf_extract(std::span<const std::uint8_t> salt,
                                          std::span<const std::uint8_t> ikm);

// RFC 5869 HKDF-SHA256 Expand: generates OKM of requested length (up to 255 * 32).
std::vector<std::uint8_t> hkdf_expand(std::span<const std::uint8_t> prk,
                                      std::span<const std::uint8_t> info,
                                      std::size_t length);

struct RootKeys {
    std::array<std::uint8_t, 32> root{};
    std::array<std::uint8_t, 32> beacon{};
    std::array<std::uint8_t, 32> auth{};
};

// Derives K_root, K_beacon, K_auth from PSK (protocol.md §5.3).
RootKeys derive_root_keys(std::span<const std::uint8_t> psk);

struct SessionKeys {
    std::array<std::uint8_t, 32> k_i2r{}; // Initiator -> Responder key
    std::array<std::uint8_t, 32> k_r2i{}; // Responder -> Initiator key
    std::array<std::uint8_t, 32> th{};    // Transcript hash
};

// Derives K_i2r, K_r2i and transcript hash TH from K_root and raw HELLO payloads.
SessionKeys derive_session_keys(std::span<const std::uint8_t> k_root,
                                std::span<const std::uint8_t> hello_i_raw,
                                std::span<const std::uint8_t> hello_r_raw);

// Builds 24-byte nonce from 64-bit sequence counter: counter (little-endian) || 16 zero bytes.
std::array<std::uint8_t, 24> make_nonce(std::uint64_t counter);

// Encrypts plaintext using XChaCha20-Poly1305 with 24-byte nonce and associated data.
// Output contains ciphertext followed by 16-byte Poly1305 authentication tag.
std::vector<std::uint8_t> aead_encrypt(std::span<const std::uint8_t> key,
                                       std::uint64_t counter,
                                       std::span<const std::uint8_t> ad,
                                       std::span<const std::uint8_t> plaintext);

// Decrypts ciphertext (including 16-byte tag) using XChaCha20-Poly1305.
Result<std::vector<std::uint8_t>> aead_decrypt(std::span<const std::uint8_t> key,
                                               std::uint64_t counter,
                                               std::span<const std::uint8_t> ad,
                                               std::span<const std::uint8_t> ciphertext_and_tag);

// Computes 16-byte beacon MAC: HMAC-SHA256(K_beacon, "LDB1" || flags || body)[0:16]
std::array<std::uint8_t, 16> compute_beacon_mac(std::span<const std::uint8_t> k_beacon,
                                                std::uint8_t flags,
                                                std::span<const std::uint8_t> body);

// Verifies 16-byte beacon MAC.
bool verify_beacon_mac(std::span<const std::uint8_t> k_beacon,
                       std::uint8_t flags,
                       std::span<const std::uint8_t> body,
                       std::span<const std::uint8_t, 16> mac);

// Computes AUTH handshake MAC: HMAC-SHA256(K_auth, role || TH)
// role is 'I' (0x49) for Initiator, 'R' (0x52) for Responder.
std::array<std::uint8_t, 32> compute_auth_mac(std::span<const std::uint8_t> k_auth,
                                              char role,
                                              std::span<const std::uint8_t, 32> th);

// Verifies AUTH handshake MAC in constant time.
bool verify_auth_mac(std::span<const std::uint8_t> k_auth,
                     char role,
                     std::span<const std::uint8_t, 32> th,
                     std::span<const std::uint8_t, 32> expected_mac);

}  // namespace ldns::crypto
