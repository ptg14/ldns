#include "ui/widgets/input_box.hpp"

#include <algorithm>
#include <sstream>
#include "ui/term/char_width.hpp"
#include "ui/term/style.hpp"
#include "ui/widgets/box.hpp"

namespace ldns::ui {

InputBox::InputBox() = default;

void InputBox::insert_char(std::string_view utf8_char) {
    if (search_mode_) {
        search_query_.append(utf8_char);
        return;
    }
    if (cursor_pos_ >= text_.size()) {
        text_.append(utf8_char);
        cursor_pos_ = text_.size();
    } else {
        text_.insert(cursor_pos_, utf8_char);
        cursor_pos_ += utf8_char.size();
    }
}

void InputBox::backspace() {
    if (search_mode_) {
        if (!search_query_.empty()) {
            size_t prev = search_query_.size() - 1;
            while (prev > 0 && (static_cast<unsigned char>(search_query_[prev]) & 0xC0) == 0x80) {
                --prev;
            }
            search_query_.erase(prev);
        }
        return;
    }
    if (cursor_pos_ > 0 && !text_.empty()) {
        // Step back over UTF-8 boundary
        size_t prev = cursor_pos_ - 1;
        while (prev > 0 && (static_cast<unsigned char>(text_[prev]) & 0xC0) == 0x80) {
            --prev;
        }
        text_.erase(prev, cursor_pos_ - prev);
        cursor_pos_ = prev;
    }
}

void InputBox::delete_char() {
    if (cursor_pos_ < text_.size()) {
        size_t next = cursor_pos_ + 1;
        while (next < text_.size() && (static_cast<unsigned char>(text_[next]) & 0xC0) == 0x80) {
            ++next;
        }
        text_.erase(cursor_pos_, next - cursor_pos_);
    }
}

void InputBox::clear() {
    text_.clear();
    cursor_pos_ = 0;
    search_mode_ = false;
    search_query_.clear();
}

void InputBox::move_cursor_left() {
    if (cursor_pos_ > 0) {
        --cursor_pos_;
        while (cursor_pos_ > 0 && (static_cast<unsigned char>(text_[cursor_pos_]) & 0xC0) == 0x80) {
            --cursor_pos_;
        }
    }
}

void InputBox::move_cursor_right() {
    if (cursor_pos_ < text_.size()) {
        ++cursor_pos_;
        while (cursor_pos_ < text_.size() && (static_cast<unsigned char>(text_[cursor_pos_]) & 0xC0) == 0x80) {
            ++cursor_pos_;
        }
    }
}

void InputBox::move_cursor_home() {
    cursor_pos_ = 0;
}

void InputBox::move_cursor_end() {
    cursor_pos_ = text_.size();
}

void InputBox::newline() {
    insert_char("\n");
}

void InputBox::add_to_history(std::string text) {
    if (!text.empty()) {
        if (history_.empty() || history_.back() != text) {
            history_.push_back(std::move(text));
        }
    }
    history_index_ = -1;
}

void InputBox::history_up() {
    if (history_.empty()) return;

    if (history_index_ == -1) {
        temp_input_before_history_ = text_;
        history_index_ = static_cast<int>(history_.size()) - 1;
    } else if (history_index_ > 0) {
        --history_index_;
    }

    if (history_index_ >= 0 && history_index_ < static_cast<int>(history_.size())) {
        text_ = history_[history_index_];
        cursor_pos_ = text_.size();
    }
}

void InputBox::history_down() {
    if (history_.empty() || history_index_ == -1) return;

    ++history_index_;
    if (history_index_ >= static_cast<int>(history_.size())) {
        history_index_ = -1;
        text_ = temp_input_before_history_;
        cursor_pos_ = text_.size();
    } else {
        text_ = history_[history_index_];
        cursor_pos_ = text_.size();
    }
}

void InputBox::start_search() {
    search_mode_ = true;
    search_query_.clear();
}

void InputBox::cancel_search() {
    search_mode_ = false;
    search_query_.clear();
}

void InputBox::accept_search() {
    std::string match = current_search_match();
    if (!match.empty()) {
        text_ = match;
        cursor_pos_ = text_.size();
    }
    search_mode_ = false;
    search_query_.clear();
}

std::string InputBox::current_search_match() const {
    if (search_query_.empty()) {
        if (!history_.empty()) {
            return history_.back();
        }
        return "";
    }

    // 1. Search backwards in history
    for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
        if (it->find(search_query_) != std::string::npos) {
            return *it;
        }
    }

    // 2. Search built-in commands as smart fallback
    static const std::vector<std::string> kBuiltins = {
        "/doctor", "/status", "/resolve", "/peers", "/conflicts",
        "/logs", "/system", "/add", "/set", "/rm", "/ls",
        "/info", "/help", "/exit", "/version"
    };

    for (const auto& cmd : kBuiltins) {
        if (cmd.find(search_query_) != std::string::npos) {
            return cmd;
        }
    }

    return "";
}

void InputBox::set_text(std::string text) {
    text_ = std::move(text);
    cursor_pos_ = text_.size();
}

int InputBox::cursor_visual_col() const {
    if (search_mode_) {
        // Border '│' (1) + "(bsearch) `" (11) + search_query_ width + 1 (cursor position)
        size_t prefix_w = CharWidth::display_width("(bsearch) `");
        size_t query_w = CharWidth::display_width(search_query_);
        return static_cast<int>(1 + prefix_w + query_w + 1);
    }
    std::string prefix = "> ";
    std::string before_cursor = text_.substr(0, cursor_pos_);
    return static_cast<int>(CharWidth::display_width(prefix) + CharWidth::display_width(before_cursor) + 2);
}

std::vector<std::string> InputBox::render(size_t term_width) const {
    size_t box_width = (term_width < 10) ? 10 : term_width;

    std::vector<std::string> lines;

    if (search_mode_) {
        std::string match = current_search_match();
        std::string search_line = Style::accent("(bsearch)") + " `" + search_query_ + "`: " + Style::primary(match);
        lines.push_back(search_line);
        return Box::wrap_box(lines, box_width);
    }

    // Split text by newlines if multiline
    std::vector<std::string> text_lines;
    size_t start = 0;
    while (start < text_.size()) {
        size_t pos = text_.find('\n', start);
        if (pos == std::string::npos) {
            text_lines.push_back(text_.substr(start));
            break;
        }
        text_lines.push_back(text_.substr(start, pos - start));
        start = pos + 1;
    }
    if (text_lines.empty()) {
        text_lines.push_back("");
    }

    for (size_t i = 0; i < text_lines.size(); ++i) {
        std::string line;
        if (i == 0) {
            line += Style::accent("> ");
            if (text_.empty()) {
                line += Style::dim(placeholder_);
            } else {
                line += text_lines[i];
            }
        } else {
            line += "  " + text_lines[i];
        }
        lines.push_back(line);
    }

    return Box::wrap_box(lines, box_width);
}

}  // namespace ldns::ui
