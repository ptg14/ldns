#pragma once

#include <cstdint>
#include <string>

namespace ldns::ui {

struct TerminalSize {
    uint16_t cols = 80;
    uint16_t rows = 24;
};

// Manages raw console mode and terminal capabilities
class Terminal {
public:
    static Terminal& instance();

    // Enable raw input mode and VT processing; saves state to restore later
    bool enable_raw_mode();

    // Restore terminal to original canonical state
    void disable_raw_mode();

    // Query current terminal dimensions
    TerminalSize size() const;

    // Hide or show cursor
    void show_cursor(bool show);

    // Clear from cursor down
    void clear_below();

    // Clear current line
    void clear_line();

    // Move cursor up n lines
    void move_up(int n);

    // Move cursor down n lines
    void move_down(int n);

    // Move cursor to specific column (1-indexed)
    void move_to_col(int col);

    // Read a single character or byte (non-blocking if available or blocking with timeout)
    int read_byte();

    // Check if another byte is pending in the input buffer
    bool has_byte_pending(int timeout_ms = 0);

    bool is_raw_mode() const { return raw_mode_active_; }

    ~Terminal();

private:
    Terminal();
    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;

    bool raw_mode_active_ = false;

#if defined(_WIN32)
    unsigned long orig_in_mode_ = 0;
    unsigned long orig_out_mode_ = 0;
    unsigned int orig_in_cp_ = 0;
    unsigned int orig_out_cp_ = 0;
#else
    void* orig_termios_ = nullptr;
#endif
};

}  // namespace ldns::ui
