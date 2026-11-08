#pragma once

#include <string>
#include <vector>
#include "ui/term/char_width.hpp"
#include "ui/term/style.hpp"

namespace ldns::ui {

class Box {
public:
    struct BorderChars {
        std::string top_left;
        std::string top_right;
        std::string bottom_left;
        std::string bottom_right;
        std::string horizontal;
        std::string vertical;
    };

    static BorderChars rounded() {
        if (Style::ascii_fallback()) {
            return {"+", "+", "+", "+", "-", "|"};
        }
        return {"╭", "╮", "╰", "╯", "─", "│"};
    }

    // Wrap a list of content lines inside a rounded box of specified width
    static std::vector<std::string> wrap_box(const std::vector<std::string>& lines, size_t width) {
        std::vector<std::string> result;
        if (width < 4) width = 4;
        size_t inner_width = width - 2;

        BorderChars b = rounded();
        std::string border_color = Style::no_color() ? "" : std::string(Style::kFgBorder);
        std::string reset_color = Style::no_color() ? "" : std::string(Style::kReset);

        // Top line
        std::string top;
        top += border_color + b.top_left;
        for (size_t i = 0; i < inner_width; ++i) {
            top += b.horizontal;
        }
        top += b.top_right + reset_color;
        result.push_back(top);

        // Content lines
        for (const auto& line : lines) {
            size_t w = CharWidth::display_width(line);
            std::string content_line = border_color + b.vertical + reset_color;
            if (w <= inner_width) {
                content_line += line;
                content_line.append(inner_width - w, ' ');
            } else {
                content_line += CharWidth::clip_to_width(line, inner_width);
            }
            content_line += border_color + b.vertical + reset_color;
            result.push_back(content_line);
        }

        // Bottom line
        std::string bottom;
        bottom += border_color + b.bottom_left;
        for (size_t i = 0; i < inner_width; ++i) {
            bottom += b.horizontal;
        }
        bottom += b.bottom_right + reset_color;
        result.push_back(bottom);

        return result;
    }
};

}  // namespace ldns::ui
