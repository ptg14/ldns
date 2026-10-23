#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace ldns::ui {

class InputBox {
public:
    InputBox();

    // Text manipulation
    void insert_char(std::string_view utf8_char);
    void backspace();
    void delete_char();
    void clear();

    // Cursor movement
    void move_cursor_left();
    void move_cursor_right();
    void move_cursor_home();
    void move_cursor_end();

    // Multi-line
    void newline();

    // History management
    void add_to_history(std::string text);
    void history_up();
    void history_down();

    // Search mode (Ctrl+R)
    void start_search();
    void cancel_search();
    void accept_search();
    bool is_search_mode() const { return search_mode_; }
    std::string current_search_match() const;

    // Text accessors
    const std::string& text() const { return text_; }
    void set_text(std::string text);

    size_t cursor_pos() const { return cursor_pos_; }
    void set_placeholder(std::string placeholder) { placeholder_ = std::move(placeholder); }

    // Render the input box widget
    std::vector<std::string> render(size_t term_width) const;

    // Get cursor offset relative to the input box (for positioning terminal cursor)
    int cursor_visual_col() const;

private:
    std::string text_;
    std::string placeholder_ = "Thử \"/add myapp.lan\" hoặc gõ \"/\"...";
    size_t cursor_pos_ = 0;

    std::vector<std::string> history_;
    int history_index_ = -1;
    std::string temp_input_before_history_;

    bool search_mode_ = false;
    std::string search_query_;
};

}  // namespace ldns::ui
