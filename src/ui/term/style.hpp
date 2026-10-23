#pragma once

#include <cstdlib>
#include <string>
#include <string_view>

namespace ldns::ui {

// Color and text decoration tokens following Claude Code / Terracotta palette
class Style {
public:
    static const char* safe_getenv(const char* name) {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4996)
        return std::getenv(name);
#pragma warning(pop)
#else
        return std::getenv(name);
#endif
    }

    static bool no_color() {
        const char* env = safe_getenv("NO_COLOR");
        return env != nullptr && env[0] != '\0';
    }

    static bool ascii_fallback() {
        const char* env = safe_getenv("LDNS_ASCII");
        return env != nullptr && env[0] != '\0';
    }

    // ANSI escape sequences
    static constexpr std::string_view kReset = "\033[0m";
    static constexpr std::string_view kBold = "\033[1m";
    static constexpr std::string_view kDim = "\033[2m";
    static constexpr std::string_view kUnderline = "\033[4m";

    // 24-bit RGB Truecolor escape sequences
    // Terracotta (Cam đất) - signature accent
    static constexpr std::string_view kFgAccent = "\033[38;2;217;119;6m";
    // Warm Amber / Highlight
    static constexpr std::string_view kFgPrimary = "\033[38;2;245;158;11m";
    // Success Emerald Green
    static constexpr std::string_view kFgSuccess = "\033[38;2;16;185;129m";
    // Warning Amber/Yellow
    static constexpr std::string_view kFgWarning = "\033[38;2;251;191;36m";
    // Error Crimson Red
    static constexpr std::string_view kFgError = "\033[38;2;239;68;68m";
    // Slate Gray / Muted
    static constexpr std::string_view kFgMuted = "\033[38;2;107;114;128m";
    // Border Dark Slate
    static constexpr std::string_view kFgBorder = "\033[38;2;75;85;99m";
    // Crisp White text
    static constexpr std::string_view kFgWhite = "\033[38;2;249;250;251m";

    // Helpers to wrap text
    static std::string accent(std::string_view text) {
        return wrap(kFgAccent, text);
    }

    static std::string primary(std::string_view text) {
        return wrap(kFgPrimary, text);
    }

    static std::string success(std::string_view text) {
        return wrap(kFgSuccess, text);
    }

    static std::string warning(std::string_view text) {
        return wrap(kFgWarning, text);
    }

    static std::string error(std::string_view text) {
        return wrap(kFgError, text);
    }

    static std::string muted(std::string_view text) {
        return wrap(kFgMuted, text);
    }

    static std::string dim(std::string_view text) {
        return wrap(kDim, text);
    }

    static std::string bold(std::string_view text) {
        return wrap(kBold, text);
    }

    // Glyphs with fallback
    static std::string glyph_spinner() {
        return ascii_fallback() ? "*" : "✻";
    }

    static std::string glyph_bullet() {
        return ascii_fallback() ? "o" : "⏺";
    }

    static std::string glyph_indent() {
        return ascii_fallback() ? "  |_ " : "  ⎿ ";
    }

    static std::string glyph_cursor() {
        return ascii_fallback() ? "> " : "❯ ";
    }

    static std::string glyph_dot() {
        return ascii_fallback() ? "[*]" : "●";
    }

    static std::string glyph_check() {
        return ascii_fallback() ? "[v]" : "✔";
    }

    static std::string glyph_cross() {
        return ascii_fallback() ? "[x]" : "✖";
    }

    static std::string glyph_warn() {
        return ascii_fallback() ? "[!]" : "⚠";
    }

private:
    static std::string wrap(std::string_view prefix, std::string_view text) {
        if (no_color()) {
            return std::string(text);
        }
        return std::string(prefix) + std::string(text) + std::string(kReset);
    }
};

}  // namespace ldns::ui
