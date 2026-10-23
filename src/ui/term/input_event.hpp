#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include "ui/term/terminal.hpp"

namespace ldns::ui {

enum class KeyType {
    None,
    Character,
    Enter,
    Backspace,
    Delete,
    Tab,
    Esc,
    Up,
    Down,
    Left,
    Right,
    Home,
    End,
    PageUp,
    PageDown,
    CtrlC,
    CtrlD,
    CtrlL,
    CtrlR,
    Resize
};

struct InputEvent {
    KeyType type = KeyType::None;
    std::string text;  // UTF-8 character string if type == Character
};

class InputParser {
public:
    static InputEvent read_next_event(Terminal& term, TerminalSize* last_size = nullptr) {
        while (true) {
            if (last_size) {
                TerminalSize cur = term.size();
                if (cur.cols != last_size->cols || cur.rows != last_size->rows) {
                    *last_size = cur;
                    // Debounce rapid continuous resize events while dragging window border
                    auto start_drag = std::chrono::steady_clock::now();
                    while (std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - start_drag).count() < 30) {
                        TerminalSize next_sz = term.size();
                        if (next_sz.cols != last_size->cols || next_sz.rows != last_size->rows) {
                            *last_size = next_sz;
                            start_drag = std::chrono::steady_clock::now();
                        }
                        if (term.has_byte_pending(5)) {
                            break;
                        }
                    }
                    return {KeyType::Resize, ""};
                }
            }

            if (term.has_byte_pending(20)) {
                break;
            }

            if (!last_size) {
                // Blocking read if size tracking is not requested
                break;
            }
        }

        int c = term.read_byte();
        if (c < 0) {
            return {KeyType::None, ""};
        }

        // Special control keys
        if (c == 3) {
            return {KeyType::CtrlC, ""};
        }
        if (c == 4) {
            return {KeyType::CtrlD, ""};
        }
        if (c == 12) {
            return {KeyType::CtrlL, ""};
        }
        if (c == 18) {
            return {KeyType::CtrlR, ""};
        }
        if (c == 9) {
            return {KeyType::Tab, ""};
        }
        if (c == 10 || c == 13) {
            return {KeyType::Enter, ""};
        }
        if (c == 8 || c == 127) {
            return {KeyType::Backspace, ""};
        }

#if defined(_WIN32)
        // Windows special keys: 0 or 224 prefix for arrows/home/end
        if (c == 0 || c == 224) {
            int ext = term.read_byte();
            switch (ext) {
                case 72: return {KeyType::Up, ""};
                case 80: return {KeyType::Down, ""};
                case 75: return {KeyType::Left, ""};
                case 77: return {KeyType::Right, ""};
                case 71: return {KeyType::Home, ""};
                case 79: return {KeyType::End, ""};
                case 83: return {KeyType::Delete, ""};
                case 73: return {KeyType::PageUp, ""};
                case 81: return {KeyType::PageDown, ""};
                default: return {KeyType::None, ""};
            }
        }
#endif

        if (c == 27) {  // Escape sequence
            if (!term.has_byte_pending(35)) {
                return {KeyType::Esc, ""};
            }
            int c2 = term.read_byte();
            if (c2 == '[') {
                if (!term.has_byte_pending(35)) {
                    return {KeyType::Esc, ""};
                }
                int c3 = term.read_byte();
                switch (c3) {
                    case 'A': return {KeyType::Up, ""};
                    case 'B': return {KeyType::Down, ""};
                    case 'C': return {KeyType::Right, ""};
                    case 'D': return {KeyType::Left, ""};
                    case 'H': return {KeyType::Home, ""};
                    case 'F': return {KeyType::End, ""};
                    case '1':
                    case '7': {
                        if (term.has_byte_pending(20)) (void)term.read_byte();
                        return {KeyType::Home, ""};
                    }
                    case '4':
                    case '8': {
                        if (term.has_byte_pending(20)) (void)term.read_byte();
                        return {KeyType::End, ""};
                    }
                    case '3': {
                        if (term.has_byte_pending(20)) (void)term.read_byte();
                        return {KeyType::Delete, ""};
                    }
                    case '5': {
                        if (term.has_byte_pending(20)) (void)term.read_byte();
                        return {KeyType::PageUp, ""};
                    }
                    case '6': {
                        if (term.has_byte_pending(20)) (void)term.read_byte();
                        return {KeyType::PageDown, ""};
                    }
                    default:
                        return {KeyType::None, ""};
                }
            } else if (c2 == 'O') {
                if (!term.has_byte_pending(35)) {
                    return {KeyType::Esc, ""};
                }
                int c3 = term.read_byte();
                switch (c3) {
                    case 'A': return {KeyType::Up, ""};
                    case 'B': return {KeyType::Down, ""};
                    case 'C': return {KeyType::Right, ""};
                    case 'D': return {KeyType::Left, ""};
                    case 'H': return {KeyType::Home, ""};
                    case 'F': return {KeyType::End, ""};
                    default: return {KeyType::None, ""};
                }
            }
            return {KeyType::Esc, ""};
        }

        // Regular character (including UTF-8 multibyte sequence)
        std::string utf8_char;
        utf8_char.push_back(static_cast<char>(c));

        size_t extra_bytes = 0;
        unsigned char uc = static_cast<unsigned char>(c);
        if ((uc & 0xE0) == 0xC0) extra_bytes = 1;
        else if ((uc & 0xF0) == 0xE0) extra_bytes = 2;
        else if ((uc & 0xF8) == 0xF0) extra_bytes = 3;

        for (size_t i = 0; i < extra_bytes; ++i) {
            int next_b = term.read_byte();
            if (next_b >= 0) {
                utf8_char.push_back(static_cast<char>(next_b));
            }
        }

        return {KeyType::Character, utf8_char};
    }
};

}  // namespace ldns::ui
