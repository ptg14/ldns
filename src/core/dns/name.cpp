#include "core/dns/name.hpp"

#include <algorithm>
#include <array>

namespace ldns::dns {
namespace {

bool is_label_char(unsigned char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '_';
}

bool has_non_ascii(std::string_view text) noexcept {
    return std::any_of(text.begin(), text.end(),
                       [](char c) { return static_cast<unsigned char>(c) >= 0x80; });
}

char to_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

Result<std::vector<std::uint8_t>> parse_presentation(std::string_view text, bool allow_wildcard) {
    std::vector<std::uint8_t> wire;
    wire.reserve(text.size() + 2);

    if (text.empty()) {
        return Error{0, "empty name"};
    }
    if (text == ".") {
        wire.push_back(0);
        return wire;
    }

    std::size_t pos = 0;
    std::size_t label_index = 0;
    while (pos <= text.size()) {
        const std::size_t dot = text.find('.', pos);
        const std::size_t end = dot == std::string_view::npos ? text.size() : dot;
        std::string_view label = text.substr(pos, end - pos);

        if (label.empty()) {
            return Error{1, "empty label in name"};
        }

        std::string encoded;
        if (label == "*") {
            if (!allow_wildcard || label_index != 0) {
                return Error{2, "wildcard label is only allowed as the leftmost label"};
            }
            encoded = "*";
        } else if (has_non_ascii(label)) {
            // Lowercase ASCII letters before encoding: "Aü" and "aü" are the
            // same label, and RFC 3490 case-folds before punycode. Without
            // this, parsing "xn--A-eha" again would produce a different name.
            std::string lowered(label);
            std::transform(lowered.begin(), lowered.end(), lowered.begin(), to_lower);
            auto puny = punycode_encode(lowered);
            if (!puny) return puny.error();
            encoded = "xn--" + puny.value();
            std::transform(encoded.begin(), encoded.end(), encoded.begin(), to_lower);
        } else {
            encoded.assign(label.begin(), label.end());
            std::transform(encoded.begin(), encoded.end(), encoded.begin(), to_lower);
            for (const char c : encoded) {
                if (!is_label_char(static_cast<unsigned char>(c))) {
                    return Error{3, "invalid character in label"};
                }
                if (c == '*') {
                    return Error{2, "wildcard label must be exactly '*' and leftmost"};
                }
            }
        }

        if (encoded.size() > kMaxLabel) {
            return Error{4, "label longer than 63 bytes"};
        }

        wire.push_back(static_cast<std::uint8_t>(encoded.size()));
        wire.insert(wire.end(), encoded.begin(), encoded.end());
        ++label_index;

        if (dot == std::string_view::npos) break;
        pos = dot + 1;
        if (pos == text.size()) break;  // tolerate a single trailing dot
    }

    wire.push_back(0);
    if (wire.size() > kMaxNameWire) {
        return Error{5, "name longer than 255 bytes on the wire"};
    }
    return wire;
}

Result<std::vector<std::uint8_t>> parse_wire(std::span<const std::uint8_t> message,
                                             std::size_t& offset) {
    std::vector<std::uint8_t> wire;
    std::size_t cursor = offset;
    int depth = 0;
    bool jumped = false;

    while (true) {
        if (cursor >= message.size()) {
            return Error{10, "name runs past end of message"};
        }
        const std::uint8_t len = message[cursor];

        if (len == 0) {
            wire.push_back(0);
            if (!jumped) offset = cursor + 1;
            break;
        }

        if ((len & 0xC0) == 0xC0) {
            if (cursor + 1 >= message.size()) {
                return Error{11, "truncated compression pointer"};
            }
            if (++depth > kMaxCompressionDepth) {
                return Error{12, "compression pointer depth exceeded"};
            }
            const std::size_t target =
                (static_cast<std::size_t>(len & 0x3F) << 8) | message[cursor + 1];
            if (target >= message.size()) {
                return Error{13, "compression pointer out of bounds"};
            }
            if (!jumped) {
                offset = cursor + 2;
                jumped = true;
            }
            if (target >= cursor) {
                return Error{14, "compression pointer must point backwards"};
            }
            cursor = target;
            continue;
        }

        if ((len & 0xC0) != 0) {
            return Error{15, "reserved label type"};
        }

        if (cursor + 1 + len > message.size()) {
            return Error{16, "label runs past end of message"};
        }
        wire.push_back(len);
        for (std::size_t i = 1; i <= len; ++i) {
            const unsigned char raw = message[cursor + i];
            wire.push_back(static_cast<std::uint8_t>(to_lower(static_cast<char>(raw))));
        }
        if (wire.size() + 1 > kMaxNameWire) {
            return Error{5, "name longer than 255 bytes on the wire"};
        }
        cursor += 1 + len;
    }

    return wire;
}

}  // namespace

Result<Name> Name::validate_wire(std::vector<std::uint8_t> wire, bool allow_wildcard) {
    if (wire.size() > kMaxNameWire) {
        return Error{5, "name longer than 255 bytes on the wire"};
    }
    if (wire.empty() || wire.back() != 0) {
        return Error{20, "name is not zero-terminated"};
    }

    std::size_t pos = 0;
    std::size_t index = 0;
    while (pos < wire.size() - 1) {
        const std::uint8_t len = wire[pos];
        if (len == 0 || len > kMaxLabel || pos + 1 + len >= wire.size()) {
            return Error{21, "malformed wire name"};
        }
        const bool wildcard = len == 1 && wire[pos + 1] == '*';
        if (wildcard && (!allow_wildcard || index != 0)) {
            return Error{2, "wildcard label is only allowed as the leftmost label"};
        }
        if (!wildcard) {
            for (std::size_t i = 1; i <= len; ++i) {
                if (!is_label_char(wire[pos + i])) {
                    return Error{3, "invalid character in label"};
                }
            }
        }
        pos += 1 + len;
        ++index;
    }
    if (pos != wire.size() - 1) {
        return Error{21, "malformed wire name"};
    }
    return Name(std::move(wire));
}

Result<Name> Name::from_presentation(std::string_view text, bool allow_wildcard) {
    auto wire = parse_presentation(text, allow_wildcard);
    if (!wire) return wire.error();
    return validate_wire(std::move(wire.value()), allow_wildcard);
}

Result<Name> Name::make_wildcard(const Name& parent) {
    if (parent.is_root()) {
        // "*." is not a meaningful zone owner.
        return Error{6, "cannot build a wildcard under the root"};
    }
    if (parent.is_wildcard()) {
        return Error{7, "wildcard cannot be nested"};
    }
    std::vector<std::uint8_t> wire;
    wire.reserve(parent.wire().size() + 2);
    wire.push_back(1);
    wire.push_back('*');
    wire.insert(wire.end(), parent.wire().begin(), parent.wire().end());
    if (wire.size() > kMaxNameWire) {
        return Error{5, "name longer than 255 bytes on the wire"};
    }
    return validate_wire(std::move(wire), /*allow_wildcard=*/true);
}

Result<Name> Name::from_wire(std::span<const std::uint8_t> message, std::size_t& offset) {
    if (offset >= message.size()) {
        return Error{10, "name offset past end of message"};
    }
    auto wire = parse_wire(message, offset);
    if (!wire) return wire.error();
    return validate_wire(std::move(wire.value()), /*allow_wildcard=*/true);
}

std::string Name::to_string() const {
    if (is_root()) return ".";
    std::string out;
    out.reserve(wire_.size());
    std::size_t pos = 0;
    while (wire_[pos] != 0) {
        const std::size_t len = wire_[pos];
        if (!out.empty()) out.push_back('.');
        out.append(reinterpret_cast<const char*>(wire_.data() + pos + 1), len);
        pos += 1 + len;
    }
    return out;
}

std::size_t Name::label_count() const noexcept {
    std::size_t count = 0;
    std::size_t pos = 0;
    while (pos + 1 <= wire_.size() && wire_[pos] != 0) {
        ++count;
        pos += 1 + wire_[pos];
    }
    return count;
}

Name Name::parent() const {
    if (is_root()) return Name();
    const std::size_t first_len = wire_[0];
    return Name(std::vector<std::uint8_t>(wire_.begin() + static_cast<std::ptrdiff_t>(1 + first_len),
                                          wire_.end()));
}

bool Name::is_suffix_of(const Name& other) const noexcept {
    if (wire_.size() > other.wire_.size()) return false;
    const std::size_t target = other.wire_.size() - wire_.size();

    // A raw byte-suffix match is not enough: a label's length byte (1..63)
    // is itself a byte that can appear inside a label ('-' is 45, '0'..'9'
    // are 48..57). Without this check "a*48" would look like a suffix of
    // "x0a*48" because the inner '0' happens to equal the length byte.
    // The match must start exactly on a label boundary of `other`.
    std::size_t cursor = 0;
    while (cursor < other.wire_.size() && other.wire_[cursor] != 0) {
        if (cursor == target) break;
        if (cursor > target) return false;
        cursor += 1 + static_cast<std::size_t>(other.wire_[cursor]);
    }
    if (cursor != target) return false;

    return std::equal(wire_.begin(), wire_.end(),
                      other.wire_.end() - static_cast<std::ptrdiff_t>(wire_.size()));
}

bool Name::wildcard_matches(const Name& candidate) const noexcept {
    if (!is_wildcard()) return false;
    return parent().is_suffix_of(candidate) && candidate != parent();
}

// ---------------------------------------------------------------------------
// Punycode (RFC 3492), UTF-8 input.
// ---------------------------------------------------------------------------
namespace {

constexpr std::uint32_t kBase = 36;
constexpr std::uint32_t kTMin = 1;
constexpr std::uint32_t kTMax = 26;
constexpr std::uint32_t kSkew = 38;
constexpr std::uint32_t kDamp = 700;
constexpr std::uint32_t kInitialBias = 72;
constexpr std::uint32_t kInitialN = 128;
constexpr std::uint32_t kMaxCodePoint = 0x10FFFF;

char encode_digit(std::uint32_t d) {
    // 0..25 -> a..z, 26..35 -> 0..9
    return static_cast<char>(d < 26 ? 'a' + d : '0' + (d - 26));
}

std::uint32_t adapt(std::uint32_t delta, std::uint32_t num_points, bool first_time) {
    delta = first_time ? delta / kDamp : delta / 2;
    delta += delta / num_points;
    std::uint32_t k = 0;
    while (delta > ((kBase - kTMin) * kTMax) / 2) {
        delta /= kBase - kTMin;
        k += kBase;
    }
    return k + (((kBase - kTMin + 1) * delta) / (delta + kSkew));
}

Result<std::vector<std::uint32_t>> decode_utf8(std::string_view text) {
    std::vector<std::uint32_t> out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        const auto lead = static_cast<unsigned char>(text[i]);
        std::uint32_t cp = 0;
        std::size_t extra = 0;
        std::uint32_t min_cp = 0;
        if (lead < 0x80) {
            cp = lead;
            extra = 0;
        } else if (lead >= 0xC2 && lead <= 0xDF) {
            cp = lead & 0x1F;
            extra = 1;
            min_cp = 0x80;
        } else if ((lead & 0xF0) == 0xE0) {
            cp = lead & 0x0F;
            extra = 2;
            min_cp = 0x800;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            cp = lead & 0x07;
            extra = 3;
            min_cp = 0x10000;
        } else {
            return Error{30, "invalid UTF-8 lead byte"};
        }
        if (i + extra >= text.size() && extra > 0) {
            return Error{31, "truncated UTF-8 sequence"};
        }
        for (std::size_t k = 1; k <= extra; ++k) {
            const auto cont = static_cast<unsigned char>(text[i + k]);
            if ((cont & 0xC0) != 0x80) {
                return Error{32, "invalid UTF-8 continuation byte"};
            }
            cp = (cp << 6) | (cont & 0x3F);
        }
        if (cp < min_cp || (cp >= 0xD800 && cp <= 0xDFFF) || cp > kMaxCodePoint) {
            return Error{33, "code point out of range"};
        }
        out.push_back(cp);
        i += extra + 1;
    }
    return out;
}

}  // namespace

Result<std::string> punycode_encode(std::string_view utf8) {
    auto decoded = decode_utf8(utf8);
    if (!decoded) return decoded.error();
    const auto& input = decoded.value();

    std::string output;
    std::size_t basic_count = 0;
    for (const auto cp : input) {
        if (cp < 0x80) {
            output.push_back(static_cast<char>(cp));
            ++basic_count;
        }
    }
    if (basic_count > 0) output.push_back('-');

    std::uint32_t n = kInitialN;
    std::uint32_t delta = 0;
    std::uint32_t bias = kInitialBias;
    std::size_t handled = basic_count;

    while (handled < input.size()) {
        std::uint32_t m = kMaxCodePoint;
        for (const auto cp : input) {
            if (cp >= n && cp < m) m = cp;
        }
        if (m - n > (0xFFFFFFFFu - delta) / static_cast<std::uint32_t>(handled + 1)) {
            return Error{34, "punycode overflow"};
        }
        delta += (m - n) * static_cast<std::uint32_t>(handled + 1);
        n = m;

        for (const auto cp : input) {
            if (cp < n) {
                if (++delta == 0) return Error{34, "punycode overflow"};
            }
            if (cp == n) {
                std::uint32_t q = delta;
                for (std::uint32_t k = kBase;; k += kBase) {
                    const std::uint32_t t = k <= bias ? kTMin : (k >= bias + kTMax ? kTMax : k - bias);
                    if (q < t) break;
                    output.push_back(encode_digit(t + (q - t) % (kBase - t)));
                    q = (q - t) / (kBase - t);
                }
                output.push_back(encode_digit(q));
                bias = adapt(delta, static_cast<std::uint32_t>(handled + 1), handled == basic_count);
                delta = 0;
                ++handled;
            }
        }
        ++delta;
        ++n;
    }
    return output;
}

}  // namespace ldns::dns
