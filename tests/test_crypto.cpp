#include "doctest.hpp"

#include "core/net/crypto.hpp"

#include <string_view>
#include <vector>

using namespace ldns::crypto;

TEST_CASE("Crypto initialization and SHA-256") {
    REQUIRE(init().has_value());

    std::string_view hello = "hello world";
    std::span<const std::uint8_t> data(
        reinterpret_cast<const std::uint8_t*>(hello.data()), hello.size());

    auto hash = sha256(data);
    CHECK(hash.size() == 32);

    auto net = net_hash("office");
    CHECK(net.size() == 8);
}

TEST_CASE("HKDF-SHA256 Extract and Expand") {
    REQUIRE(init().has_value());

    std::string_view ikm = "my-shared-secret-key-1234567890";
    std::string_view salt = "ldns-test-salt";
    std::span<const std::uint8_t> ikm_span(
        reinterpret_cast<const std::uint8_t*>(ikm.data()), ikm.size());
    std::span<const std::uint8_t> salt_span(
        reinterpret_cast<const std::uint8_t*>(salt.data()), salt.size());

    auto prk = hkdf_extract(salt_span, ikm_span);
    CHECK(prk.size() == 32);

    std::string_view info = "test-info";
    std::span<const std::uint8_t> info_span(
        reinterpret_cast<const std::uint8_t*>(info.data()), info.size());

    auto okm = hkdf_expand(prk, info_span, 64);
    REQUIRE(okm.size() == 64);

    // Deterministic output
    auto okm2 = hkdf_expand(prk, info_span, 64);
    CHECK(okm == okm2);
}

TEST_CASE("PSK Root and Session Key Derivation") {
    REQUIRE(init().has_value());

    std::vector<std::uint8_t> psk(32, 0x42);
    auto root_keys = derive_root_keys(psk);
    CHECK(root_keys.root.size() == 32);
    CHECK(root_keys.beacon.size() == 32);
    CHECK(root_keys.auth.size() == 32);

    std::vector<std::uint8_t> hello_i = {1, 2, 3, 4};
    std::vector<std::uint8_t> hello_r = {5, 6, 7, 8};

    auto session_keys = derive_session_keys(root_keys.root, hello_i, hello_r);
    CHECK(session_keys.k_i2r.size() == 32);
    CHECK(session_keys.k_r2i.size() == 32);
    CHECK(session_keys.k_i2r != session_keys.k_r2i);
    CHECK(session_keys.th.size() == 32);
}

TEST_CASE("AEAD Encryption and Decryption") {
    REQUIRE(init().has_value());

    std::vector<std::uint8_t> key(32, 0xAA);
    std::uint64_t counter = 42;
    std::vector<std::uint8_t> ad = {0x00, 0x00, 0x00, 0x0C}; // 4-byte BE length
    std::string_view message = "Secret payload message!";
    std::span<const std::uint8_t> plaintext(
        reinterpret_cast<const std::uint8_t*>(message.data()), message.size());

    auto encrypted = aead_encrypt(key, counter, ad, plaintext);
    CHECK(encrypted.size() == plaintext.size() + 16);

    auto decrypted = aead_decrypt(key, counter, ad, encrypted);
    REQUIRE(decrypted.has_value());
    CHECK(decrypted.value() == std::vector<std::uint8_t>(plaintext.begin(), plaintext.end()));

    // Tag tampering detection
    encrypted[0] ^= 0x01;
    auto bad_decrypt = aead_decrypt(key, counter, ad, encrypted);
    CHECK_FALSE(bad_decrypt.has_value());

    // Counter mismatch detection (replay/reorder protection)
    encrypted[0] ^= 0x01; // restore ciphertext
    auto wrong_counter = aead_decrypt(key, counter + 1, ad, encrypted);
    CHECK_FALSE(wrong_counter.has_value());
}

TEST_CASE("Beacon MAC Calculation & Verification") {
    REQUIRE(init().has_value());

    std::vector<std::uint8_t> k_beacon(32, 0x55);
    std::uint8_t flags = 0x01;
    std::vector<std::uint8_t> body = {0x80, 0x01, 0x02};

    auto mac = compute_beacon_mac(k_beacon, flags, body);
    CHECK(mac.size() == 16);

    CHECK(verify_beacon_mac(k_beacon, flags, body, mac));

    // Tampered body
    std::vector<std::uint8_t> bad_body = {0x80, 0x01, 0x03};
    CHECK_FALSE(verify_beacon_mac(k_beacon, flags, bad_body, mac));
}

TEST_CASE("AUTH Handshake MAC Calculation & Verification") {
    REQUIRE(init().has_value());

    std::vector<std::uint8_t> k_auth(32, 0x77);
    std::array<std::uint8_t, 32> th{};
    th.fill(0x33);

    auto mac_i = compute_auth_mac(k_auth, 'I', th);
    auto mac_r = compute_auth_mac(k_auth, 'R', th);
    CHECK(mac_i != mac_r);

    CHECK(verify_auth_mac(k_auth, 'I', th, mac_i));
    CHECK(verify_auth_mac(k_auth, 'R', th, mac_r));
    CHECK_FALSE(verify_auth_mac(k_auth, 'I', th, mac_r));
}
