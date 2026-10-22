// libFuzzer harness for the DNS message parser (PLAN M1, SPEC §8, §17).
//
// Build with Clang and -fsanitize=fuzzer,address,undefined (see
// scripts/wsl-fuzz.sh). Invariant: parsing arbitrary bytes must never crash,
// read out of bounds, or allocate unbounded memory; successfully parsed
// messages must re-encode and re-parse to an equivalent message.

#include <cstddef>
#include <cstdint>
#include <span>

#include "core/dns/message.hpp"

namespace {

void check_round_trip(const ldns::dns::Message& message) {
    auto encoded = message.encode(65535);
    if (!encoded) return;  // oversized messages are rejected by design
    auto reparsed = ldns::dns::Message::parse(encoded.value());
    if (!reparsed) {
        // Encoder produced something its own parser rejects: a real bug.
        __builtin_trap();
    }
    const auto& second = reparsed.value();
    if (second.id != message.id || second.questions.size() != message.questions.size() ||
        second.answers.size() != message.answers.size()) {
        __builtin_trap();
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::uint8_t> packet(data, size);
    auto parsed = ldns::dns::Message::parse(packet);
    if (parsed) {
        check_round_trip(parsed.value());
    }
    return 0;
}
