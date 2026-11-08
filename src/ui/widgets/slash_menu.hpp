#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ldns::ui {

struct CommandDesc {
    std::string name;        // e.g. "/firewall" or "status" (label in menu)
    std::string completion;  // e.g. "/firewall" or "/firewall status" (inserted into input box)
    std::string syntax;      // e.g. "/firewall status"
    std::string description; // e.g. "Xem trạng thái các cổng tường lửa LAN"

    CommandDesc() = default;

    CommandDesc(std::string n, std::string syn, std::string desc)
        : name(n), completion(std::move(n)), syntax(std::move(syn)), description(std::move(desc)) {}

    CommandDesc(std::string n, std::string comp, std::string syn, std::string desc)
        : name(std::move(n)), completion(std::move(comp)), syntax(std::move(syn)), description(std::move(desc)) {}
};

class SlashMenu {
public:
    SlashMenu();

    // Set filter query (e.g. "/re", "/firewall ", "/network key")
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
    std::vector<CommandDesc> subcommands_;
    std::vector<CommandDesc> filtered_;
    size_t selected_index_ = 0;
};

}  // namespace ldns::ui
