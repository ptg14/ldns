#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ldns::ui {

// Utility to calculate display columns (cell width) in terminal, handling UTF-8 and ANSI escapes.
class CharWidth {
public:
    // Strip ANSI escape codes from a string to measure its visible text length
    static std::string strip_ansi(std::string_view input) {
        std::string out;
        out.reserve(input.size());
        bool in_escape = false;
        for (size_t i = 0; i < input.size(); ++i) {
            char c = input[i];
            if (c == '\033') {
                in_escape = true;
            } else if (in_escape) {
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '~') {
                    in_escape = false;
                }
            } else {
                out.push_back(c);
            }
        }
        return out;
    }

    // Returns visible display column width of a UTF-8 string (ignoring ANSI color sequences)
    static size_t display_width(std::string_view utf8_str) {
        std::string plain = strip_ansi(utf8_str);
        size_t cols = 0;
        size_t i = 0;
        while (i < plain.size()) {
            uint8_t b = static_cast<uint8_t>(plain[i]);
            size_t seq_len = 1;
            uint32_t codepoint = b;

            if ((b & 0x80) == 0) {
                seq_len = 1;
                codepoint = b;
            } else if ((b & 0xE0) == 0xC0 && i + 1 < plain.size()) {
                seq_len = 2;
                codepoint = ((b & 0x1F) << 6) | (static_cast<uint8_t>(plain[i + 1]) & 0x3F);
            } else if ((b & 0xF0) == 0xE0 && i + 2 < plain.size()) {
                seq_len = 3;
                codepoint = ((b & 0x0F) << 12) |
                            ((static_cast<uint8_t>(plain[i + 1]) & 0x3F) << 6) |
                            (static_cast<uint8_t>(plain[i + 2]) & 0x3F);
            } else if ((b & 0xF8) == 0xF0 && i + 3 < plain.size()) {
                seq_len = 4;
                codepoint = ((b & 0x07) << 18) |
                            ((static_cast<uint8_t>(plain[i + 1]) & 0x3F) << 12) |
                            ((static_cast<uint8_t>(plain[i + 2]) & 0x3F) << 6) |
                            (static_cast<uint8_t>(plain[i + 3]) & 0x3F);
            }

            // Zero-width combining characters (common Vietnamese diacritics / accents in NFD)
            if (is_combining(codepoint)) {
                // width = 0
            } else if (is_wide(codepoint)) {
                cols += 2;
            } else {
                cols += 1;
            }

            i += seq_len;
        }
        return cols;
    }

    // Safely clip string to fit in max_cols display width, padding with spaces if shorter
    static std::string clip_to_width(std::string_view input, size_t max_cols) {
        std::string out;
        size_t cols = 0;
        size_t i = 0;
        bool in_escape = false;
        while (i < input.size()) {
            char c = input[i];
            if (c == '\033') {
                in_escape = true;
                out.push_back(c);
                ++i;
                continue;
            }
            if (in_escape) {
                out.push_back(c);
                if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '~') {
                    in_escape = false;
                }
                ++i;
                continue;
            }

            uint8_t b = static_cast<uint8_t>(c);
            size_t seq_len = 1;
            uint32_t codepoint = b;
            if ((b & 0x80) == 0) {
                seq_len = 1;
                codepoint = b;
            } else if ((b & 0xE0) == 0xC0 && i + 1 < input.size()) {
                seq_len = 2;
                codepoint = ((b & 0x1F) << 6) | (static_cast<uint8_t>(input[i + 1]) & 0x3F);
            } else if ((b & 0xF0) == 0xE0 && i + 2 < input.size()) {
                seq_len = 3;
                codepoint = ((b & 0x0F) << 12) |
                            ((static_cast<uint8_t>(input[i + 1]) & 0x3F) << 6) |
                            (static_cast<uint8_t>(input[i + 2]) & 0x3F);
            } else if ((b & 0xF8) == 0xF0 && i + 3 < input.size()) {
                seq_len = 4;
                codepoint = ((b & 0x07) << 18) |
                            ((static_cast<uint8_t>(input[i + 1]) & 0x3F) << 12) |
                            ((static_cast<uint8_t>(input[i + 2]) & 0x3F) << 6) |
                            (static_cast<uint8_t>(input[i + 3]) & 0x3F);
            }

            size_t char_w = is_combining(codepoint) ? 0 : (is_wide(codepoint) ? 2 : 1);
            if (cols + char_w > max_cols) {
                break;
            }
            cols += char_w;
            for (size_t s = 0; s < seq_len && i + s < input.size(); ++s) {
                out.push_back(input[i + s]);
            }
            i += seq_len;
        }

        out += "\033[0m";
        if (cols < max_cols) {
            out.append(max_cols - cols, ' ');
        }
        return out;
    }

private:
    static bool is_combining(uint32_t cp) {
        // Combining Diacritical Marks (0x0300 - 0x036F)
        // Combining Diacritical Marks Extended (0x1AB0 - 0x1AFF)
        // Combining Diacritical Marks Supplement (0x1DC0 - 0x1DFF)
        return (cp >= 0x0300 && cp <= 0x036F) ||
               (cp >= 0x1AB0 && cp <= 0x1AFF) ||
               (cp >= 0x1DC0 && cp <= 0x1DFF);
    }

    static bool is_wide(uint32_t cp) {
        // Basic CJK and Emoji ranges
        return (cp >= 0x1100 &&
                (cp <= 0x115F ||
                 cp == 0x2329 || cp == 0x232A ||
                 (cp >= 0x2E80 && cp <= 0xA4CF && cp != 0x303F) ||
                 (cp >= 0xAC00 && cp <= 0xD7A3) ||
                 (cp >= 0xF900 && cp <= 0xFAFF) ||
                 (cp >= 0xFE10 && cp <= 0xFE19) ||
                 (cp >= 0xFE30 && cp <= 0xFE6F) ||
                 (cp >= 0xFF00 && cp <= 0xFF60) ||
                 (cp >= 0xFFE0 && cp <= 0xFFE6) ||
                 (cp >= 0x1F300 && cp <= 0x1F64F) ||
                 (cp >= 0x1F900 && cp <= 0x1F9FF)));
    }
};

}  // namespace ldns::ui
