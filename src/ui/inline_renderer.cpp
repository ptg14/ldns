#include "ui/inline_renderer.hpp"

#include <iostream>
#include "ui/term/char_width.hpp"

namespace ldns::ui {

InlineRenderer::InlineRenderer(Terminal& term) : term_(term) {}

void InlineRenderer::clear_dynamic_zone() {
    if (last_dynamic_height_ > 0) {
        term_.show_cursor(false);
        if (cursor_y_ > 0) {
            term_.move_up(cursor_y_);
        }
        term_.move_to_col(1);
        term_.clear_line();
        term_.clear_below();
        last_dynamic_height_ = 0;
        last_rendered_width_ = 0;
        cursor_y_ = 0;
    }
}

void InlineRenderer::prepare_for_resize(size_t new_cols) {
    if (last_dynamic_height_ > 0) {
        term_.show_cursor(false);
        size_t mult = 1;
        if (last_rendered_width_ > 0 && new_cols > 0 && new_cols < last_rendered_width_) {
            mult = (last_rendered_width_ + new_cols - 1) / new_cols;
        }
        int rows_to_move = static_cast<int>(cursor_y_ * mult);
        if (rows_to_move > 0) {
            term_.move_up(rows_to_move);
        }
        term_.move_to_col(1);
        term_.clear_line();
        term_.clear_below();
        last_dynamic_height_ = 0;
        last_rendered_width_ = 0;
        cursor_y_ = 0;
    }
}

void InlineRenderer::print_scrollback_line(const std::string& line) {
    clear_dynamic_zone();
    std::cout << line << "\n" << std::flush;
}

void InlineRenderer::print_scrollback_lines(const std::vector<std::string>& lines) {
    if (lines.empty()) return;
    clear_dynamic_zone();
    std::string buf;
    for (const auto& line : lines) {
        buf += line;
        buf += "\n";
    }
    std::cout << buf << std::flush;
}

void InlineRenderer::render_dynamic_zone(const std::vector<std::string>& dynamic_lines,
                                         int cursor_line_offset,
                                         int cursor_col) {
    if (dynamic_lines.empty()) return;

    std::string buf;
    buf.reserve(4096);

    // Hide cursor during frame update
    buf += "\033[?25l";

    // Reposition cursor from current cursor_y_ to line 0 of dynamic zone
    if (last_dynamic_height_ > 0 && cursor_y_ > 0) {
        buf += "\033[" + std::to_string(cursor_y_) + "A";
    }

    // Overwrite each line in-place with line-clearing \033[K to eliminate screen-blanking flicker
    for (size_t i = 0; i < dynamic_lines.size(); ++i) {
        buf += "\r";
        buf += dynamic_lines[i];
        buf += "\033[K";
        if (i + 1 < dynamic_lines.size()) {
            buf += "\n";
        }
    }

    // If previous dynamic zone had more lines (e.g. menu closed), clear leftover lines below
    if (last_dynamic_height_ > dynamic_lines.size()) {
        size_t extra = last_dynamic_height_ - dynamic_lines.size();
        for (size_t i = 0; i < extra; ++i) {
            buf += "\n\033[2K";
        }
        buf += "\033[" + std::to_string(extra) + "A";
    }

    last_dynamic_height_ = dynamic_lines.size();
    last_rendered_width_ = CharWidth::display_width(dynamic_lines[0]);

    // Reposition cursor to designated line and column inside dynamic zone
    int end_line = static_cast<int>(dynamic_lines.size() - 1);
    if (cursor_line_offset >= 0 && cursor_line_offset < static_cast<int>(dynamic_lines.size())) {
        int lines_up = end_line - cursor_line_offset;
        if (lines_up > 0) {
            buf += "\033[" + std::to_string(lines_up) + "A";
        }
        if (cursor_col >= 1) {
            buf += "\033[" + std::to_string(cursor_col) + "G";
        }
        cursor_y_ = cursor_line_offset;
        buf += "\033[?25h"; // Show cursor
    } else {
        cursor_y_ = end_line;
    }

    // Single atomic write to console
    std::cout << buf << std::flush;
}

void InlineRenderer::render_full_screen(const std::vector<std::string>& dynamic_lines,
                                        int cursor_line_offset,
                                        int cursor_col) {
    if (dynamic_lines.empty()) return;

    std::string buf;
    buf.reserve(4096);

    // Hide cursor and jump to top-left (1,1) in one shot
    buf += "\033[?25l\033[1;1H";

    for (size_t i = 0; i < dynamic_lines.size(); ++i) {
        buf += "\r";
        buf += dynamic_lines[i];
        buf += "\033[K";
        if (i + 1 < dynamic_lines.size()) {
            buf += "\n";
        }
    }

    // Clear any extra rows below the active content
    buf += "\033[J";

    last_dynamic_height_ = dynamic_lines.size();
    last_rendered_width_ = CharWidth::display_width(dynamic_lines[0]);

    int end_line = static_cast<int>(dynamic_lines.size() - 1);
    if (cursor_line_offset >= 0 && cursor_line_offset < static_cast<int>(dynamic_lines.size())) {
        int lines_up = end_line - cursor_line_offset;
        if (lines_up > 0) {
            buf += "\033[" + std::to_string(lines_up) + "A";
        }
        if (cursor_col >= 1) {
            buf += "\033[" + std::to_string(cursor_col) + "G";
        }
        cursor_y_ = cursor_line_offset;
        buf += "\033[?25h";
    } else {
        cursor_y_ = end_line;
    }

    std::cout << buf << std::flush;
}

}  // namespace ldns::ui
