#pragma once

// dns::Name — normalized domain name (SPEC §5.1).
//
// Stored in wire format: a sequence of length-prefixed labels terminated by a
// zero byte. All names are lowercase; presentation input is normalized
// (trailing dot removed, IDN labels punycode-encoded). The trailing dot in
// presentation form is optional on input and always printed for the root only.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/util/result.hpp"

namespace ldns::dns {

inline constexpr std::size_t kMaxNameWire = 255;  // includes root byte
inline constexpr std::size_t kMaxLabel = 63;
inline constexpr int kMaxCompressionDepth = 32;

class Name {
public:
    Name() = default;  // root

    // Parses presentation form. `allow_wildcard` permits a leading "*" label.
    // Returns an error for empty labels, invalid characters, over-long names,
    // or wildcard labels in any position other than the leftmost.
    static Result<Name> from_presentation(std::string_view text, bool allow_wildcard = true);

    // Builds "*.<parent>" (e.g. make_wildcard(Name("app.lan")) -> "*.app.lan").
    static Result<Name> make_wildcard(const Name& parent);

    // Parses wire form starting at `offset`, following compression pointers.
    // `offset` is advanced past the name as it appears in `message` (not past
    // pointer targets). On error, `offset` is unspecified.
    static Result<Name> from_wire(std::span<const std::uint8_t> message, std::size_t& offset);

    bool is_root() const noexcept { return wire_.size() == 1; }
    bool is_wildcard() const noexcept { return !is_root() && (wire_[0] == 1 && wire_[1] == '*'); }

    std::span<const std::uint8_t> wire() const noexcept { return wire_; }

    // Presentation form, labels joined with '.', no trailing dot (root = ".").
    std::string to_string() const;

    // Number of labels, excluding the root label.
    std::size_t label_count() const noexcept;

    // Name with the leftmost label removed (root's parent is root).
    Name parent() const;

    // True if `child` is strictly below this name (this name is a suffix).
    bool is_suffix_of(const Name& other) const noexcept;

    // True if this wildcard name ("*.x") matches `candidate`: candidate must
    // share the suffix and have at least one extra label (SPEC §9.1).
    bool wildcard_matches(const Name& candidate) const noexcept;

    friend bool operator==(const Name& a, const Name& b) noexcept { return a.wire_ == b.wire_; }
    friend bool operator!=(const Name& a, const Name& b) noexcept { return !(a == b); }
    friend bool operator<(const Name& a, const Name& b) noexcept { return a.wire_ < b.wire_; }

private:
    explicit Name(std::vector<std::uint8_t> wire) : wire_(std::move(wire)) {}

    static Result<Name> validate_wire(std::vector<std::uint8_t> wire, bool allow_wildcard);

    std::vector<std::uint8_t> wire_{0};
};

// RFC 3492 punycode encoding without the "xn--" prefix. Input is UTF-8; returns
// an error for invalid UTF-8. Unicode NFC normalization is not applied.
Result<std::string> punycode_encode(std::string_view utf8);

}  // namespace ldns::dns

// Hash support for unordered containers keyed by name.
template <>
struct std::hash<ldns::dns::Name> {
    std::size_t operator()(const ldns::dns::Name& name) const noexcept {
        const auto bytes = name.wire();
        // FNV-1a 64.
        std::size_t h = 1469598103934665603ull;
        for (const auto b : bytes) {
            h ^= b;
            h *= 1099511628211ull;
        }
        return h;
    }
};
