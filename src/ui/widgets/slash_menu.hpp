#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace ldns::ui {

struct CommandDesc {
    std::string name;        // e.g. "/add"
    std::string syntax;      // e.g. "/add <tên> [đích…]"
    std::string description; // e.g. "Tạo domain (đích mặc định self)"
};

class SlashMenu {
public:
    SlashMenu();

    // Set filter query (e.g. "/re")
    void set_query(std::string_view query);

    // Get current filtered list
    const std::vector<CommandDesc>& filtered_items() const { return filtered_; }

    // Navigation
    void move_selection_up();
    void move_selection_down();
    size_t selected_index() const { return selected_index_; }

    // Get currently selected command
    const CommandDesc* selected_command() const;

    // Render popup box lines
    std::vector<std::string> render(size_t term_width) const;

private:
    std::vector<CommandDesc> commands_;
    std::vector<CommandDesc> filtered_;
    size_t selected_index_ = 0;
};

}  // namespace ldns::ui
