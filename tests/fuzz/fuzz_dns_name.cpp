// libFuzzer harness for dns::Name (presentation and wire parsing).
// Invariants: no crash/UB; any successfully parsed name must round-trip
// through its presentation form and wire form.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "core/dns/name.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view text(reinterpret_cast<const char*>(data), size);

    if (auto from_text = ldns::dns::Name::from_presentation(text); from_text) {
        const std::string printed = from_text.value().to_string();
        auto again = ldns::dns::Name::from_presentation(printed);
        if (!again || again.value() != from_text.value()) {
            __builtin_trap();  // normalization is not idempotent
        }
    }

    const std::span<const std::uint8_t> packet(data, size);
    if (!packet.empty()) {
        std::size_t offset = 0;
        if (auto from_wire = ldns::dns::Name::from_wire(packet, offset); from_wire) {
            if (offset > packet.size()) __builtin_trap();
            const auto wire = from_wire.value().wire();
            std::size_t reparse_offset = 0;
            const std::span<const std::uint8_t> standalone(wire.data(), wire.size());
            auto reparsed = ldns::dns::Name::from_wire(standalone, reparse_offset);
            if (!reparsed || reparsed.value() != from_wire.value()) {
                __builtin_trap();
            }
        }
    }
    return 0;
}
