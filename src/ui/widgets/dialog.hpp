#pragma once

#include <string>
#include <vector>

namespace ldns::ui {

struct DialogOption {
    int key_num;
    std::string text;
};

class Dialog {
public:
    Dialog(std::string title, std::vector<std::string> description, std::vector<DialogOption> options);

    void move_up();
    void move_down();
    bool select_num(int num);

    int selected_num() const;
    bool is_active() const { return active_; }
    void close() { active_ = false; }

    std::vector<std::string> render(size_t term_width) const;

private:
    std::string title_;
    std::vector<std::string> description_;
    std::vector<DialogOption> options_;
    size_t selected_idx_ = 0;
    bool active_ = true;
};

}  // namespace ldns::ui
