#include "ui/widgets/status_line.hpp"

#include <sstream>
#include "ui/term/char_width.hpp"
#include "ui/term/style.hpp"

namespace ldns::ui {

std::string StatusLine::render(const StatusStats& stats, std::string_view hint, size_t term_width) {
    std::string dot = stats.online ? Style::success(Style::glyph_dot())
                                   : Style::error(Style::glyph_dot());

    std::ostringstream left_ss;
    left_ss << "  " << dot << "  "
            << stats.peer_count << " peer " << Style::dim("·") << " "
            << stats.domain_count << " domain " << Style::dim("·") << " "
            << stats.conflict_count << " xung đột";

    std::string left = left_ss.str();
    std::string right = Style::dim(hint.empty() ? "ctrl+c×2 thoát" : hint);

    size_t left_w = CharWidth::display_width(left);
    size_t right_w = CharWidth::display_width(right);

    if (left_w + right_w + 2 > term_width) {
        // Condensed version for narrow terminals
        std::ostringstream cond_ss;
        cond_ss << "  " << dot << "  " << stats.peer_count << "p/" << stats.domain_count << "d";
        std::string cond_left = cond_ss.str();
        size_t cond_w = CharWidth::display_width(cond_left);
        if (cond_w + right_w + 1 <= term_width) {
            size_t pad = term_width - (cond_w + right_w);
            return cond_left + std::string(pad, ' ') + right;
        }
        return cond_left;
    }

    size_t padding = (term_width >= left_w + right_w) ? (term_width - (left_w + right_w)) : 0;
    return left + std::string(padding, ' ') + right;
}

}  // namespace ldns::ui
