#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <vector>
#include "ui/inline_renderer.hpp"
#include "ui/term/terminal.hpp"
#include "ui/widgets/conflict_view.hpp"
#include "ui/widgets/dialog.hpp"
#include "ui/widgets/info_view.hpp"
#include "ui/widgets/input_box.hpp"
#include "ui/widgets/log_view.hpp"
#include "ui/widgets/network_view.hpp"
#include "ui/widgets/peers_view.hpp"
#include "ui/widgets/slash_menu.hpp"
#include "ui/widgets/status_line.hpp"
#include "ui/widgets/upstream_view.hpp"
#include "ui/widgets/welcome_panel.hpp"

namespace ldns {
class CoreApi;
}

namespace ldns::ui {

class TuiApp {
public:
    explicit TuiApp(std::unique_ptr<CoreApi> core = nullptr);
    ~TuiApp();

    // Runs the main interactive TUI event loop
    int run();

    // Injects a real-time event to the scrollback (simulating EventBus notification)
    void inject_event(const std::string& event_text);

private:
    void render_all(bool full_screen_refresh = false);
    void redraw_full_view();
    void handle_command(const std::string& cmd_line);
    void handle_logs_follow();

    void add_scrollback(const std::string& line);
    void add_scrollback(const std::vector<std::string>& lines);

    Terminal& term_;
    InlineRenderer renderer_;

    std::unique_ptr<CoreApi> core_;

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
    bool suppressing_self_events_ = false;

    struct ScrollbackEntry {
        bool is_welcome = false;
        std::vector<std::string> lines;
    };
    std::vector<ScrollbackEntry> scrollback_entries_;

    // Interactive UI state (syncs with CoreApi if present, otherwise uses mock data)
    std::vector<std::pair<std::string, std::string>> my_domains_;
    std::vector<PeerNode> my_peers_;
    std::vector<UpstreamServer> my_upstreams_;
    NetworkInfo network_info_;
    std::vector<ConflictDomain> mock_conflicts_;
    std::vector<QueryLogEntry> mock_logs_;
};

}  // namespace ldns::ui
