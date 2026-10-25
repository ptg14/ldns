#include "core/net/crypto.hpp"

#include <sodium.h>

#include <algorithm>
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

}  // namespace ldns::crypto
