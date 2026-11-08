#pragma once

#include <cstddef>
#include <string>
#include <vector>
#include "ui/term/terminal.hpp"

namespace ldns::ui {

class InlineRenderer {
public:
    InlineRenderer(Terminal& term);

    // Prints an immutable line to the scrollback above the dynamic active zone
    void print_scrollback_line(const std::string& line);
    void print_scrollback_lines(const std::vector<std::string>& lines);

    // Renders the dynamic active zone at the bottom
    // dynamic_lines: all lines of the active zone (slash popup + input box + status line)
    // cursor_line_offset: 0-indexed line from top of dynamic zone where cursor should sit
    // cursor_col: 1-indexed column where cursor should sit
    void render_dynamic_zone(const std::vector<std::string>& dynamic_lines,
                             int cursor_line_offset = -1,
                             int cursor_col = -1);

    // Renders full active view from row 1 column 1 without screen blanking (flicker-free resize/Ctrl+L)
    void render_full_screen(const std::vector<std::string>& dynamic_lines,
                            int cursor_line_offset = -1,
                            int cursor_col = -1);

    // Clears the active dynamic zone completely (e.g. on shutdown or before full screen clear)
    void clear_dynamic_zone();

    // Resets internal cursor tracking after full-screen clearing (\033[2J)
    void reset() {
        last_dynamic_height_ = 0;
        last_rendered_width_ = 0;
        cursor_y_ = 0;
    }

    // Sets dynamic state after full-view redraw so subsequent typing is localized and flicker-free
    void set_dynamic_state(size_t height, int cursor_y, size_t width) {
        last_dynamic_height_ = height;
        cursor_y_ = cursor_y;
        last_rendered_width_ = width;
    }

    // Cleans up dynamic lines on terminal resize before drawing next frame
    void prepare_for_resize(size_t new_cols);

    size_t last_rendered_height() const { return last_dynamic_height_; }

private:
    Terminal& term_;
    size_t last_dynamic_height_ = 0;
    size_t last_rendered_width_ = 0;
    int cursor_y_ = 0; // Current 0-indexed line offset within the dynamic zone
};

}  // namespace ldns::ui
