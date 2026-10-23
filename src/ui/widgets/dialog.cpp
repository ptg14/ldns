#include "ui/widgets/dialog.hpp"

#include <algorithm>
#include "ui/term/style.hpp"
#include "ui/widgets/box.hpp"

namespace ldns::ui {

Dialog::Dialog(std::string title, std::vector<std::string> description, std::vector<DialogOption> options)
    : title_(std::move(title)), description_(std::move(description)), options_(std::move(options)) {}

void Dialog::move_up() {
    if (!options_.empty()) {
        if (selected_idx_ == 0) {
            selected_idx_ = options_.size() - 1;
        } else {
            --selected_idx_;
        }
    }
}

void Dialog::move_down() {
    if (!options_.empty()) {
        selected_idx_ = (selected_idx_ + 1) % options_.size();
    }
}

void Dialog::select_num(int num) {
    for (size_t i = 0; i < options_.size(); ++i) {
        if (options_[i].key_num == num) {
            selected_idx_ = i;
            break;
        }
    }
}

int Dialog::selected_num() const {
    if (selected_idx_ < options_.size()) {
        return options_[selected_idx_].key_num;
    }
    return -1;
}

std::vector<std::string> Dialog::render(size_t term_width) const {
    size_t box_width = std::min<size_t>(term_width, 56);
    if (box_width < 40) box_width = term_width;

    std::vector<std::string> lines;
    lines.push_back(" " + Style::bold(title_));
    for (const auto& d : description_) {
        lines.push_back(" " + Style::dim(d));
    }
    lines.push_back("");

    for (size_t i = 0; i < options_.size(); ++i) {
        bool is_selected = (i == selected_idx_);
        std::string opt_line;
        if (is_selected) {
            opt_line += "  " + Style::accent(Style::glyph_cursor()) + " ";
            opt_line += Style::bold(std::to_string(options_[i].key_num) + ". " + options_[i].text);
        } else {
            opt_line += "     " + std::to_string(options_[i].key_num) + ". " + options_[i].text;
        }
        lines.push_back(opt_line);
    }

    return Box::wrap_box(lines, box_width);
}

}  // namespace ldns::ui
