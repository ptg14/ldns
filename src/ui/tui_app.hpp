#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <vector>
#include "ui/inline_renderer.hpp"
#include "ui/term/terminal.hpp"
#include "ui/widgets/dialog.hpp"
#include "ui/widgets/input_box.hpp"
#include "ui/widgets/slash_menu.hpp"
#include "ui/widgets/status_line.hpp"
#include "ui/widgets/welcome_panel.hpp"

namespace ldns::ui {

class TuiApp {
public:
    TuiApp();
    ~TuiApp();

    // Runs the main interactive TUI event loop
    int run();

    // Injects a real-time event to the scrollback (simulating EventBus notification)
    void inject_event(const std::string& event_text);

private:
    void render_all(bool full_screen_refresh = false);
    void redraw_full_view();
    void handle_command(const std::string& cmd_line);

    void add_scrollback(const std::string& line);
    void add_scrollback(const std::vector<std::string>& lines);

    Terminal& term_;
    InlineRenderer renderer_;

    InputBox input_box_;
    SlashMenu slash_menu_;
    StatusStats stats_;
    std::string hint_;

    std::unique_ptr<Dialog> active_dialog_;
    bool show_slash_menu_ = false;
    bool running_ = true;

    int ctrl_c_count_ = 0;
    std::chrono::steady_clock::time_point last_ctrl_c_time_;

    WelcomeInfo welcome_info_;
    bool has_executed_command_ = false;

    struct ScrollbackEntry {
        bool is_welcome = false;
        std::vector<std::string> lines;
    };
    std::vector<ScrollbackEntry> scrollback_entries_;

    // Local state for mock/interactive UI demonstration
    std::vector<std::pair<std::string, std::string>> my_domains_;
};

}  // namespace ldns::ui
