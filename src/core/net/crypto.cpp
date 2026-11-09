#include "core/net/crypto.hpp"

#include <sodium.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>

namespace ldns::crypto {

namespace {

std::once_flag init_flag;
bool init_ok{false};

}  // namespace

Result<void> init() {
    std::call_once(init_flag, []() {
        if (sodium_init() >= 0) {
            init_ok = true;
        }
    });
    if (!init_ok) {
        return Error{801, "failed to initialize libsodium"};
    }
    return {};
}

std::array<std::uint8_t, 32> sha256(std::span<const std::uint8_t> data) {
    std::array<std::uint8_t, 32> out{};
    crypto_hash_sha256(out.data(), data.data(), data.size());
    return out;
}

std::array<std::uint8_t, 8> net_hash(std::string_view network_name) {
    std::string input = "ldns-net-v1:";
    input.append(network_name);

    auto hash = sha256(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(input.data()), input.size()));

    std::array<std::uint8_t, 8> out{};
    std::copy_n(hash.begin(), 8, out.begin());
    return out;
}

void random_bytes(std::span<std::uint8_t> out) {
    randombytes_buf(out.data(), out.size());
}

std::array<std::uint8_t, 32> random_bytes_32() {
    std::array<std::uint8_t, 32> out{};
    random_bytes(out);
    return out;
}

std::array<std::uint8_t, 32> hmac_sha256(std::span<const std::uint8_t> key,
                                         std::span<const std::uint8_t> data) {
    std::array<std::uint8_t, 32> out{};
    crypto_auth_hmacsha256_state state;
    crypto_auth_hmacsha256_init(&state, key.data(), key.size());
    crypto_auth_hmacsha256_update(&state, data.data(), data.size());
    crypto_auth_hmacsha256_final(&state, out.data());
    return out;
}

std::array<std::uint8_t, 32> hkdf_extract(std::span<const std::uint8_t> salt,
                                          std::span<const std::uint8_t> ikm) {
    if (salt.empty()) {
        std::array<std::uint8_t, 32> zeros{};
        return hmac_sha256(zeros, ikm);
    }
    return hmac_sha256(salt, ikm);
}

std::vector<std::uint8_t> hkdf_expand(std::span<const std::uint8_t> prk,
                                      std::span<const std::uint8_t> info,
                                      std::size_t length) {
    std::vector<std::uint8_t> okm;
    okm.reserve(length);

    std::size_t n = (length + 31) / 32;
    std::array<std::uint8_t, 32> t{};
    std::size_t t_len = 0;

    for (std::size_t i = 1; i <= n; ++i) {
        std::vector<std::uint8_t> step_input;
        step_input.reserve(t_len + info.size() + 1);
        if (t_len > 0) {
            step_input.insert(step_input.end(), t.data(), t.data() + t_len);
        }
        if (!info.empty()) {
            step_input.insert(step_input.end(), info.data(), info.data() + info.size());
        }
        step_input.push_back(static_cast<std::uint8_t>(i));

        t = hmac_sha256(prk, step_input);
        t_len = 32;

        std::size_t to_copy = std::min<std::size_t>(32, length - okm.size());
        okm.insert(okm.end(), t.begin(), t.begin() + static_cast<std::ptrdiff_t>(to_copy));
    }

    return okm;
}

RootKeys derive_root_keys(std::span<const std::uint8_t> psk) {
    RootKeys keys;
    std::string_view salt = "ldns-psk-v1";
    keys.root = hkdf_extract(
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(salt.data()), salt.size()),
        psk);

    std::string_view beacon_info = "beacon";
    auto beacon_bytes = hkdf_expand(
        keys.root,
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(beacon_info.data()), beacon_info.size()),
        32);
    std::copy_n(beacon_bytes.begin(), 32, keys.beacon.begin());

    std::string_view auth_info = "auth";
    auto auth_bytes = hkdf_expand(
        keys.root,
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(auth_info.data()), auth_info.size()),
        32);
    std::copy_n(auth_bytes.begin(), 32, keys.auth.begin());

    return keys;
}

SessionKeys derive_session_keys(std::span<const std::uint8_t> k_root,
                                std::span<const std::uint8_t> hello_i_raw,
                                std::span<const std::uint8_t> hello_r_raw) {
    SessionKeys keys;

    // TH = SHA-256(bytes payload HELLO(I) || bytes payload HELLO(R))
    std::vector<std::uint8_t> th_input;
    th_input.reserve(hello_i_raw.size() + hello_r_raw.size());
    th_input.insert(th_input.end(), hello_i_raw.begin(), hello_i_raw.end());
    th_input.insert(th_input.end(), hello_r_raw.begin(), hello_r_raw.end());
    keys.th = sha256(th_input);

    // K_i2r = HKDF-Expand(K_root, "i2r" || TH, 32)
    std::vector<std::uint8_t> i2r_info = {'i', '2', 'r'};
    i2r_info.insert(i2r_info.end(), keys.th.begin(), keys.th.end());
    auto i2r_bytes = hkdf_expand(k_root, i2r_info, 32);
    std::copy_n(i2r_bytes.begin(), 32, keys.k_i2r.begin());

    // K_r2i = HKDF-Expand(K_root, "r2i" || TH, 32)
    std::vector<std::uint8_t> r2i_info = {'r', '2', 'i'};
    r2i_info.insert(r2i_info.end(), keys.th.begin(), keys.th.end());
    auto r2i_bytes = hkdf_expand(k_root, r2i_info, 32);
    std::copy_n(r2i_bytes.begin(), 32, keys.k_r2i.begin());

    return keys;
}

std::array<std::uint8_t, 24> make_nonce(std::uint64_t counter) {
    std::array<std::uint8_t, 24> nonce{};
    // Little-endian 64-bit counter
    for (std::size_t i = 0; i < 8; ++i) {
        nonce[i] = static_cast<std::uint8_t>((counter >> (i * 8)) & 0xFF);
    }
    // remaining 16 bytes are 0
    return nonce;
}

std::vector<std::uint8_t> aead_encrypt(std::span<const std::uint8_t> key,
                                       std::uint64_t counter,
                                       std::span<const std::uint8_t> ad,
                                       std::span<const std::uint8_t> plaintext) {
    auto nonce = make_nonce(counter);
    std::vector<std::uint8_t> out(plaintext.size() + crypto_aead_xchacha20poly1305_ietf_ABYTES);
    unsigned long long clen = 0;

    crypto_aead_xchacha20poly1305_ietf_encrypt(
        out.data(), &clen,
        plaintext.data(), plaintext.size(),
        ad.data(), ad.size(),
        nullptr, nonce.data(), key.data());

    out.resize(static_cast<std::size_t>(clen));
    return out;
}

Result<std::vector<std::uint8_t>> aead_decrypt(std::span<const std::uint8_t> key,
                                               std::uint64_t counter,
                                               std::span<const std::uint8_t> ad,
                                               std::span<const std::uint8_t> ciphertext_and_tag) {
    if (ciphertext_and_tag.size() < crypto_aead_xchacha20poly1305_ietf_ABYTES) {
        return Error{802, "ciphertext too short"};
    }
    auto nonce = make_nonce(counter);
    std::vector<std::uint8_t> out(ciphertext_and_tag.size() - crypto_aead_xchacha20poly1305_ietf_ABYTES);
    unsigned long long mlen = 0;

    int ret = crypto_aead_xchacha20poly1305_ietf_decrypt(
        out.data(), &mlen,
        nullptr,
        ciphertext_and_tag.data(), ciphertext_and_tag.size(),
        ad.data(), ad.size(),
        nonce.data(), key.data());

    if (ret != 0) {
        return Error{803, "AEAD decryption failed: authentication tag mismatch"};
    }

    out.resize(static_cast<std::size_t>(mlen));
    return out;
}

std::array<std::uint8_t, 16> compute_beacon_mac(std::span<const std::uint8_t> k_beacon,
                                                std::uint8_t flags,
                                                std::span<const std::uint8_t> body) {
    std::vector<std::uint8_t> data;
    data.reserve(5 + body.size());
    data.push_back('L');
    data.push_back('D');
    data.push_back('B');
    data.push_back('1');
    data.push_back(flags);
    data.insert(data.end(), body.begin(), body.end());

    auto full_mac = hmac_sha256(k_beacon, data);
    std::array<std::uint8_t, 16> out{};
    std::copy_n(full_mac.begin(), 16, out.begin());
    return out;
}

bool verify_beacon_mac(std::span<const std::uint8_t> k_beacon,
                       std::uint8_t flags,
                       std::span<const std::uint8_t> body,
                       std::span<const std::uint8_t, 16> mac) {
    auto expected = compute_beacon_mac(k_beacon, flags, body);
    return sodium_memcmp(expected.data(), mac.data(), 16) == 0;
}

std::array<std::uint8_t, 32> compute_auth_mac(std::span<const std::uint8_t> k_auth,
                                              char role,
                                              std::span<const std::uint8_t, 32> th) {
    std::vector<std::uint8_t> data;
    data.reserve(1 + 32);
    data.push_back(static_cast<std::uint8_t>(role));
    data.insert(data.end(), th.begin(), th.end());
    return hmac_sha256(k_auth, data);
}

bool verify_auth_mac(std::span<const std::uint8_t> k_auth,
                     char role,
                     std::span<const std::uint8_t, 32> th,
                     std::span<const std::uint8_t, 32> expected_mac) {
    auto computed = compute_auth_mac(k_auth, role, th);
    return sodium_memcmp(computed.data(), expected_mac.data(), 32) == 0;
}

}  // namespace ldns::crypto
