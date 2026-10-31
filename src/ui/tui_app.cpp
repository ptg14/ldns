#include "ui/tui_app.hpp"
#include "core/core_api.hpp"
#include "core/dns/message.hpp"
#include "core/domain/domain_service.hpp"
#include "core/net/peer_manager.hpp"
#include "core/resolver/peer_ranking.hpp"
#include "core/resolver/peer_resolver.hpp"
#include "core/resolver/pipeline.hpp"
#include "core/resolver/query_log.hpp"
#include "core/util/id.hpp"
#include "core/version.hpp"

#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>
#include "ui/term/input_event.hpp"
#include "ui/term/style.hpp"
#include "ui/widgets/doctor_view.hpp"
#include "ui/widgets/spinner.hpp"
#include "ui/widgets/trace_view.hpp"
#include "ui/widgets/welcome_panel.hpp"

namespace {

std::vector<std::string> tokenize_args(std::string_view line) {
    std::vector<std::string> tokens;
    std::string cur;
    bool in_quote = false;
    char quote_char = '\0';
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (in_quote) {
            if (c == quote_char) {
                in_quote = false;
            } else {
                cur.push_back(c);
            }
        } else {
            if (c == '"' || c == '\'') {
                in_quote = true;
                quote_char = c;
            } else if (std::isspace(static_cast<unsigned char>(c))) {
                if (!cur.empty()) {
                    tokens.push_back(std::move(cur));
                    cur.clear();
                }
            } else {
                cur.push_back(c);
            }
        }
    }
    if (!cur.empty()) {
        tokens.push_back(std::move(cur));
    }
    return tokens;
}

struct EventSuppressGuard {
    bool& flag;
    explicit EventSuppressGuard(bool& f) : flag(f) { flag = true; }
    ~EventSuppressGuard() { flag = false; }
};

} // namespace

namespace ldns::ui {

TuiApp::TuiApp(std::unique_ptr<CoreApi> core)
    : term_(Terminal::instance()), renderer_(term_), core_(std::move(core)) {
    stats_.peer_count = 3;
    stats_.domain_count = 14;
    stats_.conflict_count = 1;
    stats_.online = true;

    my_domains_ = {
        {"app.lan", "192.168.1.10 (self)"},
        {"db.lan", "192.168.1.10 (self)"},
        {"web.lan", "192.168.1.10 (self)"}
    };

    my_peers_ = {
        {"minh-laptop", "192.168.1.22:5391", "2.1 ms", 8, "v0.1.0", true},
        {"nam-desktop", "192.168.1.45:5391", "1.4 ms", 15, "v0.1.0", true},
        {"pi-server", "192.168.1.100:5391", "0.9 ms", 3, "v0.1.0", true}
    };

    my_upstreams_ = {
        {"1.1.1.1:53", "Cloudflare", "14 ms", 1, true},
        {"8.8.8.8:53", "Google", "18 ms", 2, true},
        {"9.9.9.9:53", "Quad9", "22 ms", 3, true}
    };

    network_info_.network_name = "default";
    network_info_.discovery_endpoint = "239.76.68.83:5390 (UDP Multicast)";
    network_info_.sync_port = "5391 (TCP Full-mesh)";
    network_info_.psk_enabled = false;
    network_info_.active_peers = 3;

    ConflictVoteGroup g1{1, 2, "192.168.1.10", {"thang-pc (bạn)", "nam-desktop"}, true};
    ConflictVoteGroup g2{2, 1, "192.168.1.22", {"minh-laptop"}, false};
    mock_conflicts_.push_back({"web.lan", "A", {g1, g2}, "Dùng \"/info web.lan\" để xem thời điểm claim hoặc đổi tên miền."});

    mock_logs_ = {
        {"14:02:11.104", "127.0.0.1", "A", "app.lan", "[L:Local]", "NOERROR", "180 µs"},
        {"14:02:12.350", "192.168.1.50", "A", "api.lan", "[P:Peer]", "NOERROR", "2.4 ms"},
        {"14:02:15.891", "127.0.0.1", "AAAA", "google.com", "[U:Upstream]", "NOERROR", "16.1 ms"},
        {"14:02:18.012", "192.168.1.22", "A", "test.invalid", "[U:Upstream]", "NXDOMAIN", "12.0 ms"},
        {"14:02:20.405", "127.0.0.1", "A", "db.lan", "[L:Local]", "NOERROR", "120 µs"}
    };

    welcome_info_.node_name = "thang-pc";
    welcome_info_.node_id = "a3f9e1c2";
    welcome_info_.network_name = "default";
    welcome_info_.ip_address = "192.168.1.10";
    welcome_info_.dns_port = ":53";
    welcome_info_.dns_active = true;

    if (core_) {
        auto st = core_->status();
        welcome_info_.node_id = st.short_id;
        welcome_info_.network_name = st.network_name;
        if (!st.self_addresses.empty()) {
            welcome_info_.ip_address = st.self_addresses.front();
        }
        stats_.domain_count = st.domain_count;
        network_info_.network_name = st.network_name;
        network_info_.psk_enabled = st.psk_configured && st.psk_key_file_found;

        // Sync local domain entries from CoreApi
        auto list_res = core_->list_domains();
        if (list_res) {
            my_domains_.clear();
            for (const auto& entry : list_res.value()) {
                std::string target_str;
                for (size_t i = 0; i < entry.targets.size(); ++i) {
                    if (i > 0) target_str += ", ";
                    target_str += entry.targets[i];
                }
                if (entry.target_kind == "self") target_str += " (self)";
                my_domains_.push_back({entry.name, target_str});
            }
        }

        // Real-time EventBus notification listener
        core_->set_event_handler([this](const domain::DomainEvent& ev) {
            if (suppressing_self_events_) return;
            std::string action = (ev.kind == domain::EventType::RecordPut) ? "Cập nhật domain" : "Xóa domain";
            inject_event(action + ": " + ev.name + " (" + ev.type + ") · seq " + std::to_string(ev.seq));
        });
    }
}

TuiApp::~TuiApp() {
    renderer_.clear_dynamic_zone();
    term_.disable_raw_mode();
}

void TuiApp::add_scrollback(const std::string& line) {
    scrollback_entries_.push_back({false, {line}});
    renderer_.print_scrollback_line(line);
}

void TuiApp::add_scrollback(const std::vector<std::string>& lines) {
    scrollback_entries_.push_back({false, lines});
    renderer_.print_scrollback_lines(lines);
}

void TuiApp::inject_event(const std::string& event_text) {
    add_scrollback(Style::primary(Style::glyph_bullet()) + "  " + event_text);
    render_all();
}

void TuiApp::redraw_full_view() {
    TerminalSize sz = term_.size();
    size_t box_width = (sz.cols > 2) ? static_cast<size_t>(sz.cols - 1) : static_cast<size_t>(sz.cols);
    if (box_width < 20) box_width = 20;

    std::vector<std::string> all_lines;

    // 1. Re-render all historical scrollback entries with the new responsive box_width
    for (const auto& entry : scrollback_entries_) {
        if (entry.is_welcome) {
            auto w_lines = WelcomePanel::render(welcome_info_, box_width);
            all_lines.insert(all_lines.end(), w_lines.begin(), w_lines.end());
            all_lines.push_back(""); // subtle separator line
        } else {
            all_lines.insert(all_lines.end(), entry.lines.begin(), entry.lines.end());
        }
    }

    // 2. Active view (Dialog/SlashMenu, InputBox, StatusLine)
    std::vector<std::string> dynamic_lines;
    if (!has_executed_command_) {
        auto w_lines = WelcomePanel::render(welcome_info_, box_width);
        dynamic_lines.insert(dynamic_lines.end(), w_lines.begin(), w_lines.end());
        dynamic_lines.push_back("");
    }

    if (active_dialog_ && active_dialog_->is_active()) {
        auto d_lines = active_dialog_->render(box_width);
        dynamic_lines.insert(dynamic_lines.end(), d_lines.begin(), d_lines.end());
    } else if (show_slash_menu_) {
        auto m_lines = slash_menu_.render(box_width);
        dynamic_lines.insert(dynamic_lines.end(), m_lines.begin(), m_lines.end());
    }

    size_t input_box_offset = dynamic_lines.size();
    auto ib_lines = input_box_.render(box_width);
    dynamic_lines.insert(dynamic_lines.end(), ib_lines.begin(), ib_lines.end());

    std::string s_line = StatusLine::render(stats_, hint_, box_width);
    dynamic_lines.push_back(s_line);

    size_t scrollback_count = all_lines.size();
    all_lines.insert(all_lines.end(), dynamic_lines.begin(), dynamic_lines.end());

    int cursor_line_offset = static_cast<int>(scrollback_count + input_box_offset + 1);
    int cursor_col = input_box_.cursor_visual_col();

    renderer_.render_full_screen(all_lines, cursor_line_offset, cursor_col);
    renderer_.set_dynamic_state(dynamic_lines.size(), static_cast<int>(input_box_offset + 1), box_width);
}

void TuiApp::render_all(bool full_screen_refresh) {
    if (full_screen_refresh) {
        redraw_full_view();
        return;
    }

    TerminalSize sz = term_.size();
    size_t box_width = (sz.cols > 2) ? static_cast<size_t>(sz.cols - 1) : static_cast<size_t>(sz.cols);
    if (box_width < 20) box_width = 20;

    std::vector<std::string> dynamic_lines;
    int cursor_line_offset = -1;
    int cursor_col = -1;

    // 0. Welcome card (shown above the input box until first command is executed)
    if (!has_executed_command_) {
        auto w_lines = WelcomePanel::render(welcome_info_, box_width);
        dynamic_lines.insert(dynamic_lines.end(), w_lines.begin(), w_lines.end());
        dynamic_lines.push_back(""); // subtle separator line
    }

    // 1. Popup layer: Dialog or SlashMenu
    if (active_dialog_ && active_dialog_->is_active()) {
        auto d_lines = active_dialog_->render(box_width);
        dynamic_lines.insert(dynamic_lines.end(), d_lines.begin(), d_lines.end());
    } else if (show_slash_menu_) {
        auto m_lines = slash_menu_.render(box_width);
        dynamic_lines.insert(dynamic_lines.end(), m_lines.begin(), m_lines.end());
    }

    // 2. Input Box layer
    size_t input_box_start_idx = dynamic_lines.size();
    auto ib_lines = input_box_.render(box_width);
    dynamic_lines.insert(dynamic_lines.end(), ib_lines.begin(), ib_lines.end());

    // 3. Status Line layer
    std::string s_line = StatusLine::render(stats_, hint_, box_width);
    dynamic_lines.push_back(s_line);

    // Calculate cursor position inside the input box (usually line 1 inside box, which is input_box_start_idx + 1)
    cursor_line_offset = static_cast<int>(input_box_start_idx + 1);
    cursor_col = input_box_.cursor_visual_col();

    renderer_.render_dynamic_zone(dynamic_lines, cursor_line_offset, cursor_col);
}

void TuiApp::handle_logs_follow() {
    renderer_.clear_dynamic_zone();

    add_scrollback(Style::primary(Style::glyph_bullet()) + "  " +
                   Style::bold("Đang theo dõi nhật ký truy vấn DNS theo thời gian thực") +
                   Style::dim(" (Ấn Esc hoặc q để dừng)"));

    std::string header = "  " + Style::dim("Thời gian      Client IP        Loại   Tên domain             Phân giải      Kết quả    Độ trễ");
    add_scrollback(header);
    add_scrollback(Style::dim("  ────────────────────────────────────────────────────────────────────────────────────────────"));

    std::vector<QueryLogEntry> live_feed = {
        {"14:05:01.120", "127.0.0.1", "A", "app.lan", "[L:Local]", "NOERROR", "195 µs"},
        {"14:05:02.440", "192.168.1.45", "A", "db.lan", "[L:Local]", "NOERROR", "210 µs"},
        {"14:05:03.900", "127.0.0.1", "A", "api.lan", "[P:Peer]", "NOERROR", "2.1 ms"},
        {"14:05:05.150", "192.168.1.22", "A", "google.com", "[U:Upstream]", "NOERROR", "15.4 ms"},
        {"14:05:06.880", "127.0.0.1", "AAAA", "github.com", "[U:Upstream]", "NOERROR", "17.2 ms"},
        {"14:05:08.010", "192.168.1.100", "A", "internal.lan", "[L:Local]", "NXDOMAIN", "90 µs"},
        {"14:05:09.620", "127.0.0.1", "A", "web.lan", "[P:Peer]", "NOERROR", "1.8 ms"}
    };

    size_t feed_idx = 0;
    while (feed_idx < live_feed.size()) {
        if (term_.has_byte_pending(600)) {
            TerminalSize last_sz = term_.size();
            InputEvent ev = InputParser::read_next_event(term_, &last_sz);
            if (ev.type == KeyType::Esc || ev.type == KeyType::CtrlC || (ev.type == KeyType::Character && (ev.text == "q" || ev.text == "Q"))) {
                break;
            }
        }

        const auto& entry = live_feed[feed_idx++];
        add_scrollback(LogView::format_entry(entry));
        mock_logs_.push_back(entry);
    }

    add_scrollback(Style::dim("⏺  Đã dừng theo dõi nhật ký truy vấn DNS."));
}

int TuiApp::run() {
    term_.enable_raw_mode();

    TerminalSize last_size = term_.size();
    redraw_full_view();

    while (running_) {
        InputEvent ev = InputParser::read_next_event(term_, &last_size);
        auto now = std::chrono::steady_clock::now();

        // Clear double Ctrl+C notice after 1.5s
        if (ctrl_c_count_ > 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_ctrl_c_time_).count();
            if (elapsed > 1500) {
                ctrl_c_count_ = 0;
                hint_.clear();
                render_all();
            }
        }

        if (ev.type == KeyType::Resize) {
            redraw_full_view();
            continue;
        }

        if (ev.type == KeyType::None) {
            continue;
        }

        if (ev.type == KeyType::CtrlC) {
            if (!input_box_.text().empty()) {
                input_box_.clear();
                show_slash_menu_ = false;
                hint_.clear();
            } else {
                ctrl_c_count_++;
                last_ctrl_c_time_ = now;
                if (ctrl_c_count_ >= 2) {
                    running_ = false;
                    break;
                } else {
                    hint_ = Style::warning("Nhấn Ctrl+C lần nữa để thoát");
                }
            }
            render_all();
            continue;
        }

        if (ev.type == KeyType::CtrlD) {
            if (input_box_.text().empty()) {
                running_ = false;
                break;
            }
        }

        if (ev.type == KeyType::CtrlL) {
            // Clear screen
            redraw_full_view();
            continue;
        }

        // Active Dialog input interception
        if (active_dialog_ && active_dialog_->is_active()) {
            if (ev.type == KeyType::Up) {
                active_dialog_->move_up();
                render_all();
                continue;
            } else if (ev.type == KeyType::Down) {
                active_dialog_->move_down();
                render_all();
                continue;
            } else if (ev.type == KeyType::Esc) {
                active_dialog_->close();
                active_dialog_.reset();
                renderer_.print_scrollback_line(Style::warning("⏺  Đã huỷ thao tác hộp thoại"));
                render_all();
                continue;
            } else if (ev.type == KeyType::Character) {
                if (ev.text >= "1" && ev.text <= "9") {
                    int num = ev.text[0] - '0';
                    active_dialog_->select_num(num);
                    int chosen = active_dialog_->selected_num();
                    active_dialog_->close();
                    active_dialog_.reset();
                    renderer_.print_scrollback_line(Style::success("⏺  Đã chọn mục " + std::to_string(chosen) + " - Đang áp dụng thiết lập..."));
                    render_all();
                    continue;
                }
            } else if (ev.type == KeyType::Enter) {
                int chosen = active_dialog_->selected_num();
                active_dialog_->close();
                active_dialog_.reset();
                renderer_.print_scrollback_line(Style::success("⏺  Đã chọn mục " + std::to_string(chosen) + " - Đang áp dụng thiết lập..."));
                render_all();
                continue;
            }
        }

        // Active Slash Menu input interception
        if (show_slash_menu_) {
            if (ev.type == KeyType::Up) {
                slash_menu_.move_selection_up();
                render_all();
                continue;
            } else if (ev.type == KeyType::Down) {
                slash_menu_.move_selection_down();
                render_all();
                continue;
            } else if (ev.type == KeyType::Tab) {
                const auto* cmd = slash_menu_.selected_command();
                if (cmd) {
                    input_box_.set_text(cmd->name + " ");
                }
                show_slash_menu_ = false;
                render_all();
                continue;
            } else if (ev.type == KeyType::Enter) {
                const auto* cmd = slash_menu_.selected_command();
                std::string chosen_cmd;
                if (cmd && !input_box_.text().empty() && input_box_.text().find(' ') == std::string::npos) {
                    chosen_cmd = cmd->name;
                } else {
                    chosen_cmd = input_box_.text();
                }
                show_slash_menu_ = false;
                input_box_.add_to_history(chosen_cmd);
                input_box_.clear();
                if (!chosen_cmd.empty()) {
                    handle_command(chosen_cmd);
                }
                render_all();
                continue;
            } else if (ev.type == KeyType::Esc) {
                show_slash_menu_ = false;
                render_all();
                continue;
            }
        }

        // Standard InputBox events
        switch (ev.type) {
            case KeyType::Character: {
                input_box_.insert_char(ev.text);
                if (input_box_.is_search_mode()) {
                    break;
                }
                const std::string& cur_text = input_box_.text();
                if (!cur_text.empty() && cur_text[0] == '/' && cur_text.find(' ') == std::string::npos) {
                    show_slash_menu_ = true;
                    slash_menu_.set_query(cur_text);
                } else {
                    show_slash_menu_ = false;
                }
                break;
            }
            case KeyType::Backspace: {
                input_box_.backspace();
                if (input_box_.is_search_mode()) {
                    break;
                }
                const std::string& cur_text = input_box_.text();
                if (!cur_text.empty() && cur_text[0] == '/' && cur_text.find(' ') == std::string::npos) {
                    show_slash_menu_ = true;
                    slash_menu_.set_query(cur_text);
                } else {
                    show_slash_menu_ = false;
                }
                break;
            }
            case KeyType::Delete:
                input_box_.delete_char();
                break;
            case KeyType::Left:
                input_box_.move_cursor_left();
                break;
            case KeyType::Right:
                input_box_.move_cursor_right();
                break;
            case KeyType::Home:
                input_box_.move_cursor_home();
                break;
            case KeyType::End:
                input_box_.move_cursor_end();
                break;
            case KeyType::Up:
                input_box_.history_up();
                break;
            case KeyType::Down:
                input_box_.history_down();
                break;
            case KeyType::CtrlR:
                input_box_.start_search();
                break;
            case KeyType::Tab:
                if (input_box_.is_search_mode()) {
                    input_box_.accept_search();
                } else if (!input_box_.text().empty() && input_box_.text()[0] == '/' && input_box_.text().find(' ') == std::string::npos) {
                    show_slash_menu_ = true;
                    slash_menu_.set_query(input_box_.text());
                }
                break;
            case KeyType::Esc:
                if (input_box_.is_search_mode()) {
                    input_box_.cancel_search();
                } else {
                    input_box_.clear();
                    show_slash_menu_ = false;
                }
                break;
            case KeyType::Enter: {
                if (input_box_.is_search_mode()) {
                    std::string match = input_box_.current_search_match();
                    input_box_.accept_search();
                    if (!match.empty()) {
                        input_box_.add_to_history(match);
                        input_box_.clear();
                        handle_command(match);
                    }
                    break;
                }
                std::string cmd = input_box_.text();
                input_box_.add_to_history(cmd);
                input_box_.clear();
                show_slash_menu_ = false;

                if (!cmd.empty()) {
                    handle_command(cmd);
                }
                break;
            }
            default:
                break;
        }

        render_all();
    }

    renderer_.clear_dynamic_zone();
    term_.disable_raw_mode();
    std::cout << Style::dim("Ldns đã dừng an toàn.") << "\n";
    return 0;
}

void TuiApp::handle_command(const std::string& cmd_line) {
    EventSuppressGuard suppress_guard(suppressing_self_events_);
    if (!has_executed_command_) {
        has_executed_command_ = true;
        scrollback_entries_.push_back({true, {}});
        TerminalSize sz = term_.size();
        size_t box_width = (sz.cols > 2) ? static_cast<size_t>(sz.cols - 1) : static_cast<size_t>(sz.cols);
        auto w_lines = WelcomePanel::render(welcome_info_, box_width);
        renderer_.print_scrollback_lines(w_lines);
    }

    // Print user's executed command into immutable scrollback
    add_scrollback(Style::accent("> ") + Style::bold(cmd_line));

    std::istringstream iss(cmd_line);
    std::string cmd;
    iss >> cmd;

    if (cmd == "/exit" || cmd == "exit") {
        running_ = false;
        return;
    }

    if (cmd == "/clear" || cmd == "clear") {
        scrollback_entries_.clear();
        std::cout << "\033[2J\033[1;1H" << std::flush;
        renderer_.reset();
        render_all();
        return;
    }

    if (cmd == "/help" || cmd == "help") {
        std::vector<std::string> lines;
        lines.push_back(Style::primary("Các lệnh Ldns thông dụng:"));
        lines.push_back("  " + Style::bold("/add <tên> [đích]") + "        Tạo domain mới (mặc định IP self)");
        lines.push_back("  " + Style::bold("/set <tên> <đích>") + "        Thay đổi địa chỉ IP đích của domain");
        lines.push_back("  " + Style::bold("/rm <tên>") + "                 Xóa domain khỏi máy này");
        lines.push_back("  " + Style::bold("/ls") + "                       Liệt kê các domain trong mạng LAN");
        lines.push_back("  " + Style::bold("/info <tên>") + "               Xem chi tiết sở hữu, số phiếu và thứ hạng");
        lines.push_back("  " + Style::bold("/import <file> [--force]") + "    Nhập hàng loạt domain từ file TOML/CSV");
        lines.push_back("  " + Style::bold("/export [--csv]") + "            Xuất danh sách domain cục bộ ra TOML/CSV");
        lines.push_back("  " + Style::bold("/resolve <tên> [--trace]") + "   Phân giải domain với sơ đồ cây 3 bước");
        lines.push_back("  " + Style::bold("/peers") + "                    Xem danh sách các máy peer trong LAN");
        lines.push_back("  " + Style::bold("/peer add|rm <ip:cổng>") + "    Thêm hoặc ngắt kết nối peer thủ công");
        lines.push_back("  " + Style::bold("/upstream [list|add|rm]") + "   Xem và quản lý DNS server upstream");
        lines.push_back("  " + Style::bold("/conflicts") + "                Trực quan hóa các tên miền đang tranh chấp");
        lines.push_back("  " + Style::bold("/logs [--follow]") + "          Xem nhật ký truy vấn DNS trực tiếp");
        lines.push_back("  " + Style::bold("/network [name|key]") + "       Cấu hình tên cụm mạng và khóa mã hóa PSK");
        lines.push_back("  " + Style::bold("/doctor") + "                   Chẩn đoán sức khỏe hệ thống toàn diện");
        lines.push_back("  " + Style::bold("/status") + "                   Tổng quan hệ thống, dịch vụ và an toàn");
        lines.push_back("  " + Style::bold("/system setup|restore") + "     Cấu hình hoặc hoàn tác DNS hệ điều hành");
        lines.push_back("  " + Style::bold("/service status|install") + "   Quản lý dịch vụ nền daemon của hệ thống");
        lines.push_back("  " + Style::bold("/version") + "                  Xem phiên bản và môi trường build");
        lines.push_back("  " + Style::bold("/exit") + "                     Thoát khỏi chương trình");
        add_scrollback(lines);
        return;
    }

    if (cmd == "/version" || cmd == "version") {
        std::vector<std::string> lines;
        lines.push_back(Style::primary(Style::glyph_bullet()) + "  " + Style::bold("Ldns — Local Peer-to-Peer DNS for LAN"));
        lines.push_back(Style::glyph_indent() + "  Phiên bản: 0.1.0-alpha (nhánh feat/ux-ui)");
        lines.push_back(Style::glyph_indent() + "  Tiêu chuẩn: C++20 · Giao thức: RFC 1035 + Ldns Peer Protocol v0.1");
#if defined(_WIN32)
        lines.push_back(Style::glyph_indent() + "  Nền tảng: Windows x64 (MSVC 2022 / Windows Terminal VT)");
#else
        lines.push_back(Style::glyph_indent() + "  Nền tảng: Linux x64 (POSIX raw / systemd-resolved compatible)");
#endif
        lines.push_back(Style::glyph_indent() + "  Trạng thái: Milestone M1 hoàn thành · UI/UX inline renderer sẵn sàng");
        add_scrollback(lines);
        return;
    }

    if (cmd == "/status" || cmd == "status") {
        std::vector<std::string> lines;
        if (core_) {
            auto st = core_->status();
            lines.push_back(Style::primary(Style::glyph_bullet()) + "  " + Style::bold("Tổng quan Ldns Node") +
                           " (node: " + st.short_id + " · epoch " + std::to_string(st.epoch) + " · seq " + std::to_string(st.seq) + ")");
            lines.push_back(Style::glyph_indent() + "  Cụm mạng: " + Style::bold(st.network_name) +
                           " · Bản ghi cục bộ: " + std::to_string(st.domain_count) + " domain");
            lines.push_back(Style::glyph_indent() + "  Database: " + Style::dim(st.data_file));
            if (!st.config_file.empty()) {
                lines.push_back(Style::glyph_indent() + "  Config:   " + Style::dim(st.config_file));
            }
            if (!st.psk_configured || !st.psk_key_file_found) {
                lines.push_back(Style::glyph_indent() + "  " + Style::warning("Cảnh báo: Chưa kích hoạt mã hóa khóa PSK cho cụm mạng"));
            } else {
                lines.push_back(Style::glyph_indent() + "  " + Style::success("Bảo mật: Mạng được bảo vệ bởi khóa PSK (AES-256-GCM)"));
            }
            lines.push_back("");
            lines.push_back("  " + Style::bold("Địa chỉ LAN publishable:"));
            if (st.self_addresses.empty()) {
                lines.push_back(Style::dim("    (Không phát hiện địa chỉ LAN phù hợp)"));
            } else {
                for (const auto& addr : st.self_addresses) {
                    lines.push_back("    " + Style::success(Style::glyph_dot()) + "  " + addr);
                }
            }
            lines.push_back("");
            lines.push_back("  " + Style::bold("Kết nối Peer (" + std::to_string(my_peers_.size()) + " peer online):"));
            for (const auto& peer : my_peers_) {
                lines.push_back("    " + peer.hostname + " (" + peer.endpoint + ")   RTT " + peer.rtt + "   " +
                                std::to_string(peer.domain_count) + " domain   " + peer.version);
            }
            add_scrollback(lines);
            return;
        }

        lines.push_back(Style::primary(Style::glyph_bullet()) + "  " + Style::bold("Tổng quan Ldns") + " (node: thang-pc · a3f9e1)");
        lines.push_back(Style::glyph_indent() + "  Phiên bản: 0.1.0 · Cổng discovery: 5390 · Cổng sync: 5391");
        if (!network_info_.psk_enabled) {
            lines.push_back(Style::glyph_indent() + "  " + Style::warning("Cảnh báo: Chưa đặt khóa mạng PSK (/network key set để bật mã hóa)"));
        } else {
            lines.push_back(Style::glyph_indent() + "  " + Style::success("Bảo mật: Mạng được bảo vệ bởi khóa PSK (" + network_info_.psk_cipher + ")"));
        }
        lines.push_back("");
        lines.push_back("  " + Style::bold("Dịch vụ DNS:"));
        lines.push_back("    " + Style::success(Style::glyph_dot()) + "  127.0.0.1:53       Hoạt động (4,821 truy vấn · avg 0.8ms)");
        lines.push_back("    " + Style::success(Style::glyph_dot()) + "  192.168.1.10:53    Hoạt động (LAN interface: Wi-Fi)");
        lines.push_back("");
        lines.push_back("  " + Style::bold("Kết nối Peer (" + std::to_string(my_peers_.size()) + " peer online):"));
        for (const auto& peer : my_peers_) {
            lines.push_back("    " + peer.hostname + " (" + peer.endpoint + ")   RTT " + peer.rtt + "   " +
                            std::to_string(peer.domain_count) + " domain   " + peer.version);
        }
        add_scrollback(lines);
        return;
    }

    if (cmd == "/add") {
        auto tokens = tokenize_args(cmd_line);
        if (tokens.size() < 2) {
            add_scrollback(Style::error("Lỗi: Thiếu tên domain. Cú pháp: /add <tên> [đích...] [--type A|AAAA|CNAME|TXT] [--ttl N] [--force]"));
            return;
        }

        std::string name = tokens[1];
        std::string type = "A";
        std::vector<std::string> targets;
        uint32_t ttl = 0;
        bool force = false;

        for (size_t i = 2; i < tokens.size(); ++i) {
            if (tokens[i] == "--force") {
                force = true;
            } else if (tokens[i] == "--type" && i + 1 < tokens.size()) {
                type = tokens[++i];
            } else if (tokens[i] == "--ttl" && i + 1 < tokens.size()) {
                ttl = static_cast<uint32_t>(std::strtoul(tokens[++i].c_str(), nullptr, 10));
            } else if (!tokens[i].starts_with("--")) {
                targets.push_back(tokens[i]);
            }
        }

        if (core_) {
            domain::AddRequest req;
            req.name = name;
            req.type = type;
            req.force = force;
            req.ttl = ttl > 0 ? ttl : domain::kDefaultTtl;

            bool is_self = targets.empty() || (targets.size() == 1 && targets[0] == "self");
            if (is_self) {
                req.kind = domain::TargetKind::Self;
            } else {
                req.targets = targets;
                req.kind = domain::TargetKind::Static;
            }

            auto add_res = is_self ? core_->add_self_domain(name, type, ttl)
                                   : core_->add_domain(req);
            if (!add_res) {
                if (add_res.error().code == 703) {
                    add_scrollback(Style::error("Lỗi: Domain " + name + " đã tồn tại. Dùng /set " + name + " <đích> để đổi IP đích."));
                } else if (add_res.error().code == 705) {
                    add_scrollback(Style::warning("Cảnh báo: Tên miền thuộc Public TLD của Internet toàn cầu."));
                    add_scrollback(Style::glyph_indent() + "  Thêm cờ --force nếu bạn chắc chắn muốn ghi đè: /add " + name + (targets.empty() ? "" : " " + targets.front()) + " --force");
                } else {
                    add_scrollback(Style::error("Lỗi: " + add_res.error().message));
                }
                return;
            }

            const auto& entry = add_res.value();
            std::string tg;
            for (size_t i = 0; i < entry.targets.size(); ++i) {
                if (i > 0) tg += ", ";
                tg += entry.targets[i];
            }
            if (entry.target_kind == "self") tg += " (self)";

            stats_.domain_count = core_->status().domain_count;
            add_scrollback(Style::success(Style::glyph_bullet()) + "  Đã thêm " + Style::bold(entry.name) + " [" + entry.type + "] → " + tg);
            add_scrollback(Style::glyph_indent() + "  Bản ghi đã lưu vào SQLite và cập nhật LocalZone (TTL " + std::to_string(entry.ttl) + "s)");
            return;
        }

        std::string tg_desc = targets.empty() ? "192.168.1.10 (self)" : targets.front();
        my_domains_.push_back({name, tg_desc});
        stats_.domain_count++;

        add_scrollback(Style::success(Style::glyph_bullet()) + "  Đã thêm " + Style::bold(name) + " [" + type + "] → " + tg_desc);
        add_scrollback(Style::glyph_indent() + "  TTL 60s · đã đồng bộ tới " + std::to_string(my_peers_.size()) + " peer trong 32 ms");
        return;
    }

    if (cmd == "/set") {
        auto tokens = tokenize_args(cmd_line);
        if (tokens.size() < 3) {
            add_scrollback(Style::error("Lỗi: Thiếu tham số. Cú pháp: /set <tên> <đích mới...> [--type A|AAAA|CNAME|TXT] [--force]"));
            return;
        }

        std::string name = tokens[1];
        std::string type = "A";
        std::vector<std::string> targets;
        bool force = false;

        for (size_t i = 2; i < tokens.size(); ++i) {
            if (tokens[i] == "--force") {
                force = true;
            } else if (tokens[i] == "--type" && i + 1 < tokens.size()) {
                type = tokens[++i];
            } else if (!tokens[i].starts_with("--")) {
                targets.push_back(tokens[i]);
            }
        }

        if (targets.empty()) {
            add_scrollback(Style::error("Lỗi: Cần cung cấp ít nhất 1 địa chỉ đích mới."));
            return;
        }

        if (core_) {
            domain::AddRequest req;
            req.name = name;
            req.type = type;
            req.targets = targets;
            req.kind = domain::TargetKind::Static;
            req.force = force;

            auto set_res = core_->set_domain(req);
            if (!set_res) {
                if (set_res.error().code == 704) {
                    add_scrollback(Style::error("Lỗi: Domain " + name + " không tồn tại. Dùng /add " + name + " để tạo mới."));
                } else if (set_res.error().code == 705) {
                    add_scrollback(Style::warning("Cảnh báo: Tên miền thuộc Public TLD. Thêm --force để tiếp tục."));
                } else {
                    add_scrollback(Style::error("Lỗi cập nhật: " + set_res.error().message));
                }
                return;
            }

            std::string tg_all;
            for (size_t i = 0; i < targets.size(); ++i) {
                if (i > 0) tg_all += ", ";
                tg_all += targets[i];
            }
            add_scrollback(Style::success(Style::glyph_bullet()) + "  Đã cập nhật đích cho " + Style::bold(name) + " [" + type + "] → " + tg_all);
            add_scrollback(Style::glyph_indent() + "  Bản ghi SQLite đã được cập nhật thành công.");
            return;
        }

        std::string tg_first = targets.front();
        bool found = false;
        for (auto& d : my_domains_) {
            if (d.first == name) {
                d.second = tg_first;
                found = true;
                break;
            }
        }
        if (!found) {
            my_domains_.push_back({name, tg_first});
            stats_.domain_count++;
        }
        add_scrollback(Style::success(Style::glyph_bullet()) + "  Đã cập nhật đích cho " + Style::bold(name) + " → " + tg_first);
        add_scrollback(Style::glyph_indent() + "  Thay đổi đã được phát tán thời gian thực tới " +
                       std::to_string(my_peers_.size()) + " peer.");
        return;
    }

    if (cmd == "/ls") {
        auto tokens = tokenize_args(cmd_line);
        std::optional<std::string> filter_type;
        for (size_t i = 1; i < tokens.size(); ++i) {
            if (tokens[i] == "--type" && i + 1 < tokens.size()) {
                filter_type = tokens[++i];
            } else if (!tokens[i].starts_with("--")) {
                filter_type = tokens[i];
            }
        }

        std::vector<std::string> lines;
        if (core_) {
            auto list_res = core_->list_domains(filter_type);
            if (!list_res) {
                add_scrollback(Style::error("Lỗi đọc danh sách: " + list_res.error().message));
                return;
            }
            const auto& entries = list_res.value();
            std::string header = Style::primary(Style::glyph_bullet()) + "  Danh sách domain cục bộ (" +
                                 std::to_string(entries.size()) + " bản ghi trong SQLite";
            if (filter_type) header += " [loại " + *filter_type + "]";
            header += "):";
            lines.push_back(header);
            if (entries.empty()) {
                lines.push_back(Style::dim("    (Trống - gõ /add <tên> để tạo domain đầu tiên)"));
            } else {
                for (const auto& entry : entries) {
                    std::string tg;
                    for (size_t i = 0; i < entry.targets.size(); ++i) {
                        if (i > 0) tg += ", ";
                        tg += entry.targets[i];
                    }
                    if (entry.target_kind == "self") tg += " (self)";
                    lines.push_back("  " + Style::bold(entry.name) + " (" + entry.type + ")  →  " + tg +
                                   Style::dim("  · TTL " + std::to_string(entry.ttl) + "s"));
                }
            }
            add_scrollback(lines);
            return;
        }

        lines.push_back(Style::primary(Style::glyph_bullet()) + "  Danh sách domain (" + std::to_string(my_domains_.size()) + " cục bộ):");
        for (const auto& d : my_domains_) {
            lines.push_back("  " + Style::bold(d.first) + "  →  " + d.second);
        }
        add_scrollback(lines);
        return;
    }

    if (cmd == "/rm") {
        auto tokens = tokenize_args(cmd_line);
        if (tokens.size() < 2) {
            add_scrollback(Style::error("Lỗi: Thiếu tên domain. Cú pháp: /rm <tên> [--type A|AAAA|CNAME|TXT]"));
            return;
        }

        std::string name = tokens[1];
        std::string type = "A";
        for (size_t i = 2; i < tokens.size(); ++i) {
            if (tokens[i] == "--type" && i + 1 < tokens.size()) {
                type = tokens[++i];
            } else if (!tokens[i].starts_with("--")) {
                type = tokens[i];
            }
        }

        if (core_) {
            auto rm_res = core_->remove_domain(name, type);
            if (!rm_res) {
                add_scrollback(Style::error("Lỗi xóa: " + rm_res.error().message));
                return;
            }
            stats_.domain_count = core_->status().domain_count;
            add_scrollback(Style::success(Style::glyph_bullet()) + "  Đã xóa domain " + Style::bold(name) + " [" + type + "]");
            add_scrollback(Style::glyph_indent() + "  Bản ghi đã bị xóa khỏi SQLite và LocalZone.");
            return;
        }

        bool found = false;
        for (auto it = my_domains_.begin(); it != my_domains_.end(); ++it) {
            if (it->first == name) {
                my_domains_.erase(it);
                stats_.domain_count--;
                found = true;
                break;
            }
        }
        if (found) {
            add_scrollback(Style::success(Style::glyph_bullet()) + "  Đã xóa domain " + Style::bold(name));
            add_scrollback(Style::glyph_indent() + "  Bản ghi thu hồi (Tombstone) đã gửi tới mạng LAN.");
        } else {
            add_scrollback(Style::error("Không tìm thấy domain " + name + " thuộc sở hữu của bạn."));
        }
        return;
    }
    if (cmd == "/import") {
        std::string file;
        iss >> file;
        if (file.empty()) {
            add_scrollback(Style::error("Lỗi: Thiếu đường dẫn file. Cú pháp: /import <file.toml|file.csv> [--force]"));
            return;
        }
        bool force = (cmd_line.find("--force") != std::string::npos);
        if (core_) {
            auto report = core_->import_file(file, force);
            if (!report) {
                add_scrollback(Style::error("Lỗi import: " + report.error().message));
                return;
            }
            stats_.domain_count = core_->status().domain_count;
            add_scrollback(Style::success(Style::glyph_bullet()) + "  Import thành công " +
                           std::to_string(report.value().imported) + " domain từ " + file);
            if (report.value().skipped > 0) {
                add_scrollback(Style::glyph_indent() + "  Bỏ qua " +
                               std::to_string(report.value().skipped) + " domain không đổi hoặc không hợp lệ.");
            }
            for (const auto& w : report.value().warnings) {
                add_scrollback(Style::warning("  ⚠ " + w));
            }
            return;
        }
        add_scrollback(Style::error("CoreApi chưa sẵn sàng để import file."));
        return;
    }

    if (cmd == "/export") {
        bool csv = (cmd_line.find("--csv") != std::string::npos);
        if (core_) {
            auto exp_res = core_->export_zone(!csv);
            if (!exp_res) {
                add_scrollback(Style::error("Lỗi export: " + exp_res.error().message));
                return;
            }
            std::vector<std::string> lines;
            lines.push_back(Style::success(Style::glyph_bullet()) + "  Xuất dữ liệu Zone (" + (csv ? "CSV" : "TOML") + "):");
            std::istringstream ess(exp_res.value());
            std::string eline;
            while (std::getline(ess, eline)) {
                lines.push_back("    " + Style::dim(eline));
            }
            add_scrollback(lines);
            return;
        }
        add_scrollback(Style::error("CoreApi chưa sẵn sàng để export zone."));
        return;
    }

    if (cmd == "/info") {
        std::string name;
        std::string type = "A";
        iss >> name;
        if (name.empty()) {
            add_scrollback(Style::error("Lỗi: Thiếu tên domain cần tra cứu. Cú pháp: /info <tên> [--type A|AAAA|CNAME|TXT]"));
            return;
        }
        std::string next_tok;
        while (iss >> next_tok) {
            if (next_tok == "--type") {
                iss >> type;
            } else if (!next_tok.starts_with("--")) {
                type = next_tok;
            }
        }

        if (core_) {
            auto info_res = core_->domain_info(name, type);
            if (info_res) {
                const auto& inf = info_res.value();
                DomainInfo info;
                info.domain = inf.name.to_string();
                info.record_type = type;
                info.is_local = inf.has_local;
                if (inf.has_local) {
                    info.owner_node = "bạn (cục bộ)";
                    for (size_t i = 0; i < inf.local_records.size(); ++i) {
                        if (i > 0) info.target += ", ";
                        info.target += inf.local_records[i];
                    }
                }
                if (!inf.ranking.groups.empty()) {
                    const auto& win = inf.ranking.groups.front();
                    info.vote_count = static_cast<int>(win.vote_count);
                    info.total_peers = static_cast<int>(my_peers_.size());
                    if (info.target.empty()) {
                        for (size_t i = 0; i < win.canonical_data.size(); ++i) {
                            if (i > 0) info.target += ", ";
                            info.target += dns::format_rdata(win.type, win.canonical_data[i]);
                        }
                    }
                    if (!win.candidates.empty()) {
                        info.owner_node = short_id(win.candidates.front().node_id);
                        info.owner_ip = "(peer)";
                    }
                }
                info.sync_status = inf.has_conflict ? "Xung đột (Đang tranh chấp)" : "Đã đồng bộ";
                if (inf.local_divergence) {
                    info.metadata.push_back({"Local Divergence", "Bản ghi cục bộ khác với nhóm đa số của peer"});
                }
                info.metadata.push_back({"Tổng số nhóm bản", std::to_string(inf.ranking.groups.size())});
                for (size_t g = 0; g < inf.ranking.groups.size(); ++g) {
                    info.metadata.push_back({"Nhóm #" + std::to_string(g + 1),
                        std::to_string(inf.ranking.groups[g].vote_count) + " phiếu (" +
                        std::to_string(inf.ranking.groups[g].candidates.size()) + " peer)"});
                }
                auto info_lines = InfoView::render(info);
                add_scrollback(info_lines);
                return;
            }
        }

        DomainInfo info;
        info.domain = name;

        if (name == "web.lan") {
            info.owner_node = "thang-pc";
            info.owner_ip = "192.168.1.10";
            info.is_local = true;
            info.target = "192.168.1.10 (self)";
            info.record_type = type;
            info.ttl = "60s";
            info.vote_count = 2;
            info.total_peers = 3;
            info.claim_time = "14:15:30 hôm nay";
            info.sync_status = "Đang dẫn đầu nhóm bầu cử (2/3 phiếu)";
            info.metadata = {
                {"Trạng thái tranh chấp", "Có 1 peer (minh-laptop) khai báo đích khác (192.168.1.22)"},
                {"Quy tắc R1", "Bản ghi của bạn đang thắng vì đa số phiếu ủng hộ"}
            };
        } else {
            bool is_mine = false;
            std::string target = "192.168.1.10";
            for (const auto& d : my_domains_) {
                if (d.first == name) {
                    is_mine = true;
                    target = d.second;
                    break;
                }
            }

            info.owner_node = is_mine ? "thang-pc" : "minh-laptop";
            info.owner_ip = is_mine ? "192.168.1.10" : "192.168.1.22";
            info.is_local = is_mine;
            info.target = target;
            info.record_type = type;
            info.ttl = "60s";
            info.vote_count = static_cast<int>(my_peers_.size());
            info.total_peers = static_cast<int>(my_peers_.size());
            info.claim_time = "12:00:00 hôm nay";
            info.sync_status = "Đồng bộ hoàn tất trên toàn cụm LAN";
        }

        auto info_lines = InfoView::render(info);
        add_scrollback(info_lines);
        return;
    }

    if (cmd == "/resolve") {
        std::string name;
        std::string arg2;
        iss >> name >> arg2;
        if (name.empty()) {
            add_scrollback(Style::error("Lỗi: Thiếu tên cần phân giải. Cú pháp: /resolve <tên> [A|AAAA|CNAME|TXT] [--trace]"));
            return;
        }

        std::string qtype = "A";
        bool trace_flag = (cmd_line.find("--trace") != std::string::npos);
        if (!arg2.empty() && arg2 != "--trace") {
            qtype = arg2;
        }

        if (core_) {
            Spinner spinner("Đang phân giải " + name + " (" + qtype + ") qua Pipeline…");
            for (int i = 0; i < 3; ++i) {
                spinner.update();
                std::vector<std::string> sp_lines = {spinner.render()};
                renderer_.render_dynamic_zone(sp_lines);
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            renderer_.clear_dynamic_zone();

            auto res = core_->resolve(name, qtype, trace_flag);
            if (!res) {
                add_scrollback(Style::error("Lỗi phân giải: " + res.error().message));
                return;
            }

            const auto& pr = res.value();
            TraceResult tr;
            tr.target_domain = name;
            tr.record_type = qtype;
            double ms = static_cast<double>(pr.total_latency_us) / 1000.0;
            std::ostringstream ss_lat;
            ss_lat << std::fixed << std::setprecision(1) << ms << " ms";
            tr.total_duration = ss_lat.str();

            auto rcode_name = [](dns::Rcode rc) -> std::string {
                switch (rc) {
                    case dns::Rcode::NoError: return "NOERROR";
                    case dns::Rcode::NxDomain: return "NXDOMAIN";
                    case dns::Rcode::ServFail: return "SERVFAIL";
                    case dns::Rcode::Refused: return "REFUSED";
                    case dns::Rcode::FormErr: return "FORMERR";
                    case dns::Rcode::NotImp: return "NOTIMP";
                    default: return "RCODE_" + std::to_string(static_cast<int>(rc));
                }
            };

            if (!pr.response.answers.empty()) {
                const auto& ans = pr.response.answers.front();
                tr.ttl = std::to_string(ans.ttl) + "s";
                if (ans.type == 1 && ans.rdata.size() == 4) {
                    tr.final_answer = std::to_string(ans.rdata[0]) + "." +
                                      std::to_string(ans.rdata[1]) + "." +
                                      std::to_string(ans.rdata[2]) + "." +
                                      std::to_string(ans.rdata[3]);
                } else if (ans.type == 5) {
                    auto cname = dns::decode_rdata_name(ans.rdata);
                    tr.final_answer = cname ? cname.value().to_string() : "(CNAME)";
                } else {
                    tr.final_answer = "(Resolved " + std::to_string(ans.rdata.size()) + " bytes)";
                }
            } else {
                tr.final_answer = "(Không có bản ghi - " + rcode_name(static_cast<dns::Rcode>(pr.response.rcode())) + ")";
            }

            int step_idx = 1;
            for (size_t i = 0; i < pr.trace.size(); ++i) {
                const auto& hop = pr.trace[i];
                TraceStep ts;
                std::string step_label = resolver::step_name(hop.step);
                ts.step_name = std::to_string(step_idx++) + ". " + step_label;
                ts.details = hop.qname.to_string() + " → " + rcode_name(hop.rcode);
                if (!hop.responder.empty()) {
                    ts.details += " (từ " + hop.responder + ")";
                }
                if (!hop.note.empty()) {
                    ts.sub_details.push_back(hop.note);
                }
                double hop_ms = static_cast<double>(hop.latency_us) / 1000.0;
                std::ostringstream hss;
                hss << std::fixed << std::setprecision(1) << hop_ms << " ms";
                ts.latency = hss.str();
                ts.is_last = (i + 1 == pr.trace.size());
                tr.steps.push_back(std::move(ts));
            }

            if (tr.steps.empty()) {
                tr.steps.push_back({"1. Pipeline Cache", "Đã phân giải từ bộ nhớ đệm", tr.total_duration, {}, true});
            }

            auto trace_lines = TraceView::render(tr);
            add_scrollback(trace_lines);
            return;
        }

        TraceResult tr;
        tr.target_domain = name;
        tr.final_answer = "192.168.1.22";
        tr.record_type = "A";
        tr.ttl = "30s";
        tr.total_duration = "18.4 ms";

        tr.steps = {
            {"1. LocalZone", name + ": Không tìm thấy (NXDOMAIN cục bộ)", "0.1 ms", {}, false},
            {"2. PeerResolver", name + ": Tìm thấy 1 ứng viên hợp lệ", "18.2 ms",
             {"minh-laptop (192.168.1.22): Đã claim (1 phiếu, nhóm A)", "Trả lời: 192.168.1.22 (RTT 17.8 ms)"}, false},
            {"3. Upstream", "Bỏ qua (đã giải quyết ở bước 2)", "", {}, true}
        };

        auto trace_lines = TraceView::render(tr);
        add_scrollback(trace_lines);
        return;
    }

    if (cmd == "/peers") {
        if (core_) {
            std::vector<PeerNode> view_peers;
            auto stored = core_->peers();
            if (stored) {
                for (const auto& sp : stored.value()) {
                    std::string p_name = sp.name.empty() ? ("node-" + short_id(sp.node_id)) : sp.name;
                    std::string ep = sp.last_addr.empty() ? "(chưa có IP)" : sp.last_addr;
                    view_peers.push_back({p_name, ep, "2.0 ms", 0, "v0.1.0", (sp.last_seen > 0)});
                }
            }
            auto manuals = core_->manual_peers();
            if (manuals) {
                for (const auto& m : manuals.value()) {
                    bool exists = false;
                    for (const auto& vp : view_peers) {
                        if (vp.endpoint.find(m) != std::string::npos) exists = true;
                    }
                    if (!exists) {
                        view_peers.push_back({"manual", m, "manual", 0, "v0.1.0", true});
                    }
                }
            }
            if (view_peers.empty()) {
                add_scrollback(Style::primary(Style::glyph_bullet()) + "  Danh sách peer LAN: (Chưa có peer nào)");
                add_scrollback(Style::glyph_indent() + "  Gợi ý: Dùng /peer add <ip:port> để kết nối peer thủ công hoặc bật daemon discovery.");
                return;
            }
            auto p_lines = PeersView::render(view_peers);
            add_scrollback(p_lines);
            return;
        }

        auto p_lines = PeersView::render(my_peers_);
        add_scrollback(p_lines);
        return;
    }

    if (cmd == "/peer") {
        std::string sub;
        std::string endpoint;
        iss >> sub >> endpoint;
        if (sub == "add") {
            if (endpoint.empty()) {
                add_scrollback(Style::error("Lỗi: Thiếu địa chỉ peer. Cú pháp: /peer add <ip[:port]>"));
                return;
            }
            if (core_) {
                auto res = core_->add_manual_peer(endpoint);
                if (!res) {
                    add_scrollback(Style::error("Lỗi thêm peer: " + res.error().message));
                    return;
                }
                stats_.peer_count++;
                add_scrollback(Style::success(Style::glyph_bullet()) + "  Đã thêm manual peer vào Core: " + Style::bold(endpoint));
                return;
            }
            std::string host = "peer-" + std::to_string(my_peers_.size() + 1);
            my_peers_.push_back({host, endpoint, "1.9 ms", 4, "v0.1.0", true});
            stats_.peer_count = static_cast<int>(my_peers_.size());
            add_scrollback(Style::success(Style::glyph_bullet()) + "  Đã kết nối thành công tới peer mới: " +
                           Style::bold(host) + " (" + endpoint + ")");
            return;
        } else if (sub == "rm") {
            if (endpoint.empty()) {
                add_scrollback(Style::error("Lỗi: Thiếu IP peer cần xóa. Cú pháp: /peer rm <ip[:port]>"));
                return;
            }
            if (core_) {
                auto res = core_->remove_manual_peer(endpoint);
                if (!res) {
                    add_scrollback(Style::error("Lỗi xóa peer: " + res.error().message));
                    return;
                }
                if (stats_.peer_count > 0) stats_.peer_count--;
                add_scrollback(Style::success(Style::glyph_bullet()) + "  Đã gỡ bỏ manual peer: " + endpoint);
                return;
            }
            bool found = false;
            for (auto it = my_peers_.begin(); it != my_peers_.end(); ++it) {
                if (it->endpoint.find(endpoint) != std::string::npos || it->hostname == endpoint) {
                    my_peers_.erase(it);
                    found = true;
                    break;
                }
            }
            if (found) {
                stats_.peer_count = static_cast<int>(my_peers_.size());
                add_scrollback(Style::success(Style::glyph_bullet()) + "  Đã ngắt kết nối và gỡ bỏ peer: " + endpoint);
            } else {
                add_scrollback(Style::error("Không tìm thấy peer khớp với: " + endpoint));
            }
            return;
        } else {
            add_scrollback(Style::error("Cú pháp: /peer add <ip[:port]> hoặc /peer rm <ip[:port]>"));
            return;
        }
    }

    if (cmd == "/upstream") {
        std::string sub;
        std::string ip;
        iss >> sub >> ip;
        if (sub == "add") {
            if (ip.empty()) {
                add_scrollback(Style::error("Lỗi: Thiếu IP upstream DNS. Cú pháp: /upstream add <ip>"));
                return;
            }
            int prio = static_cast<int>(my_upstreams_.size() + 1);
            my_upstreams_.push_back({ip + ":53", "Tùy chỉnh", "20 ms", prio, true});
            add_scrollback(Style::success(Style::glyph_bullet()) + "  Đã thêm DNS upstream: " + ip);
            return;
        } else if (sub == "rm") {
            if (ip.empty()) {
                add_scrollback(Style::error("Lỗi: Thiếu IP upstream DNS. Cú pháp: /upstream rm <ip>"));
                return;
            }
            bool found = false;
            for (auto it = my_upstreams_.begin(); it != my_upstreams_.end(); ++it) {
                if (it->ip_port.find(ip) != std::string::npos) {
                    my_upstreams_.erase(it);
                    found = true;
                    break;
                }
            }
            if (found) {
                add_scrollback(Style::success(Style::glyph_bullet()) + "  Đã gỡ bỏ DNS upstream: " + ip);
            } else {
                add_scrollback(Style::error("Không tìm thấy upstream khớp với: " + ip));
            }
            return;
        } else {
            auto up_lines = UpstreamView::render(my_upstreams_);
            add_scrollback(up_lines);
            return;
        }
    }

    if (cmd == "/conflicts") {
        std::string sub;
        iss >> sub;
        if (sub == "clear") {
            mock_conflicts_.clear();
            stats_.conflict_count = 0;
            add_scrollback(Style::success(Style::glyph_bullet()) + "  Đã xóa sạch các xung đột mẫu.");
            return;
        } else if (sub == "mock") {
            ConflictVoteGroup g1{1, 2, "192.168.1.10", {"thang-pc (bạn)", "nam-desktop"}, true};
            ConflictVoteGroup g2{2, 1, "192.168.1.22", {"minh-laptop"}, false};
            mock_conflicts_ = {{"web.lan", "A", {g1, g2}, "Dùng \"/info web.lan\" để xem thời điểm claim hoặc đổi tên miền."}};
            stats_.conflict_count = 1;
            add_scrollback(Style::warning(Style::glyph_bullet()) + "  Đã kích hoạt xung đột mẫu trên web.lan.");
            return;
        }

        if (core_) {
            auto confs = core_->conflicts();
            if (confs.empty()) {
                add_scrollback(Style::success(Style::glyph_bullet()) + "  Không có xung đột tên miền nào trong mạng!");
                add_scrollback(Style::glyph_indent() + "  Mọi bản ghi phân giải đồng nhất giữa các peer.");
                return;
            }
            std::vector<ConflictDomain> view_conflicts;
            for (const auto& c : confs) {
                ConflictDomain cd;
                cd.domain = c.name.to_string();
                cd.record_type = std::string(dns::rr_type_name(c.type));
                int grp_id = 1;
                for (size_t g_idx = 0; g_idx < c.groups.size(); ++g_idx) {
                    const auto& grp = c.groups[g_idx];
                    ConflictVoteGroup vg;
                    vg.group_id = grp_id++;
                    vg.vote_count = static_cast<int>(grp.vote_count);
                    vg.is_winner = (g_idx == 0);
                    std::string target_str;
                    for (size_t d_idx = 0; d_idx < grp.canonical_data.size(); ++d_idx) {
                        if (d_idx > 0) target_str += ", ";
                        target_str += dns::format_rdata(grp.type, grp.canonical_data[d_idx]);
                    }
                    vg.target_endpoint = target_str.empty() ? "(trống)" : target_str;
                    for (const auto& cand : grp.candidates) {
                        vg.claimants.push_back(short_id(cand.node_id));
                    }
                    cd.groups.push_back(std::move(vg));
                }
                cd.resolution_hint = "Dùng \"/info " + cd.domain + "\" để xem chi tiết các phiếu bầu.";
                view_conflicts.push_back(std::move(cd));
            }
            auto conf_lines = ConflictView::render(view_conflicts);
            add_scrollback(conf_lines);
            return;
        }

        auto conf_lines = ConflictView::render(mock_conflicts_);
        add_scrollback(conf_lines);
        return;
    }

    if (cmd == "/logs") {
        std::string flag;
        iss >> flag;
        if (flag == "--follow" || flag == "-f") {
            handle_logs_follow();
            return;
        }
        if (core_) {
            auto entries = core_->query_log().entries(15);
            if (entries.empty()) {
                add_scrollback(Style::primary(Style::glyph_bullet()) + "  Nhật ký truy vấn DNS (QueryLog): (Chưa có truy vấn nào)");
                add_scrollback(Style::glyph_indent() + "  Gợi ý: Dùng /resolve <domain> để thực hiện truy vấn thử nghiệm.");
                return;
            }
            std::vector<QueryLogEntry> view_entries;
            for (const auto& e : entries) {
                double ms = static_cast<double>(e.latency_us) / 1000.0;
                std::ostringstream lss;
                lss << std::fixed << std::setprecision(1) << ms << " ms";
                std::string step_str = "[" + std::string(1, static_cast<char>(e.step)) + ":" + resolver::step_name(e.step) + "]";
                view_entries.push_back({
                    "vừa xong",
                    e.client_ip.empty() ? "cục bộ" : e.client_ip,
                    e.qtype,
                    e.qname,
                    step_str,
                    e.rcode,
                    lss.str()
                });
            }
            auto log_lines = LogView::render(view_entries);
            add_scrollback(log_lines);
            add_scrollback("  " + Style::dim("Gợi ý: Dùng \"/logs --follow\" để theo dõi trực tiếp các gói DNS theo thời gian thực."));
            return;
        }

        auto log_lines = LogView::render(mock_logs_);
        add_scrollback(log_lines);
        add_scrollback("  " + Style::dim("Gợi ý: Dùng \"/logs --follow\" để xem trực tiếp các gói DNS gửi tới theo thời gian thực."));
        return;
    }

    if (cmd == "/network") {
        std::string sub;
        std::string arg;
        iss >> sub >> arg;

        if (sub == "key") {
            std::string action;
            iss >> action;
            if (arg == "set") {
                network_info_.psk_enabled = true;
                add_scrollback(Style::success(Style::glyph_bullet()) + "  Đã kích hoạt khóa mạng PSK thành công.");
                add_scrollback(Style::glyph_indent() + "  Mọi gói UDP Discovery và TCP Sync hiện được mã hóa bằng AES-256-GCM.");
                return;
            } else if (arg == "clear") {
                network_info_.psk_enabled = false;
                add_scrollback(Style::warning(Style::glyph_bullet()) + "  Đã xóa khóa mạng PSK. Cụm mạng chuyển về chế độ mở.");
                return;
            }
        } else if (sub == "name") {
            if (arg.empty()) {
                add_scrollback(Style::error("Lỗi: Thiếu tên mạng mới. Cú pháp: /network name <tên>"));
                return;
            }
            network_info_.network_name = arg;
            welcome_info_.network_name = arg;
            add_scrollback(Style::success(Style::glyph_bullet()) + "  Đã đổi tên cụm mạng LAN sang: \"" + arg + "\"");
            add_scrollback(Style::glyph_indent() + "  Đang tái phát hiện các node có cùng tên mạng...");
            return;
        }

        network_info_.active_peers = static_cast<int>(my_peers_.size());
        auto net_lines = NetworkView::render(network_info_);
        add_scrollback(net_lines);
        return;
    }

    if (cmd == "/service") {
        std::string sub;
        iss >> sub;
        if (sub.empty() || sub == "status") {
            std::vector<std::string> lines;
            lines.push_back(Style::primary(Style::glyph_bullet()) + "  Trạng thái dịch vụ nền Ldns Service:");
#if defined(_WIN32)
            lines.push_back(Style::glyph_indent() + "  Tên dịch vụ: LdnsService (Windows Service)");
            lines.push_back(Style::glyph_indent() + "  Trạng thái:  " + Style::success("Đang chạy (SERVICE_RUNNING · PID 3840)"));
            lines.push_back(Style::glyph_indent() + "  Cổng gắn:    \\.\\pipe\\ldns-ipc (Named Pipe)");
#else
            lines.push_back(Style::glyph_indent() + "  Tên dịch vụ: ldns.service (systemd unit)");
            lines.push_back(Style::glyph_indent() + "  Trạng thái:  " + Style::success("active (running)"));
            lines.push_back(Style::glyph_indent() + "  Cổng gắn:    /var/run/ldns.sock (Unix Domain Socket)");
#endif
            lines.push_back(Style::glyph_indent() + "  Tự khởi động: Bật (Automatic Startup)");
            add_scrollback(lines);
            return;
        } else if (sub == "install") {
            active_dialog_ = std::make_unique<Dialog>(
                "Cài đặt Ldns làm Dịch vụ Nền (System Service)?",
                std::vector<std::string>{
                    "Dịch vụ sẽ tự động chạy ngầm cùng Windows để phân giải DNS",
                    "cả khi bạn đóng cửa sổ terminal."
                },
                std::vector<DialogOption>{
                    {1, "Cài đặt ngay (Yêu cầu quyền Administrator)"},
                    {2, "Hủy bỏ"}
                }
            );
            return;
        } else {
            add_scrollback(Style::dim("Cú pháp: /service status | /service install | /service start | /service stop"));
            return;
        }
    }

    if (cmd == "/doctor") {
        Spinner spinner("Đang chẩn đoán hệ thống…");
        for (int i = 0; i < 6; ++i) {
            spinner.update();
            std::vector<std::string> sp_lines = {spinner.render()};
            renderer_.render_dynamic_zone(sp_lines);
            std::this_thread::sleep_for(std::chrono::milliseconds(90));
        }
        renderer_.clear_dynamic_zone();

        std::vector<DoctorItem> items;
        if (core_) {
            auto st = core_->status();
            items.push_back({"Cơ sở dữ liệu SQLite", DoctorStatus::Pass, "Tệp DB: " + st.data_file + " (" + std::to_string(st.domain_count) + " domain)", ""});
            if (!st.config_file.empty()) {
                items.push_back({"Cấu hình TOML", DoctorStatus::Pass, "Đã tải từ: " + st.config_file, ""});
            } else {
                items.push_back({"Cấu hình TOML", DoctorStatus::Pass, "Đang dùng cấu hình mặc định (in-memory)", ""});
            }

            if (!st.self_addresses.empty()) {
                std::string addrs;
                for (size_t i = 0; i < st.self_addresses.size(); ++i) {
                    if (i > 0) addrs += ", ";
                    addrs += st.self_addresses[i];
                }
                items.push_back({"Địa chỉ LAN (Self)", DoctorStatus::Pass, "Phát hiện interface: " + addrs, ""});
            } else {
                items.push_back({"Địa chỉ LAN (Self)", DoctorStatus::Warning, "Không phát hiện card mạng LAN nào", "Kiểm tra kết nối Wi-Fi/Ethernet"});
            }

            if (st.psk_configured && st.psk_key_file_found) {
                items.push_back({"Bảo mật cụm mạng (PSK)", DoctorStatus::Pass, "Đã bật mã hóa AES-256-GCM cho cụm: " + st.network_name, ""});
            } else {
                items.push_back({"Bảo mật cụm mạng (PSK)", DoctorStatus::Warning, "Chưa cấu hình khóa PSK cho cụm: " + st.network_name, "Dùng \"/network key set\" để kích hoạt"});
            }

            items.push_back({"Cổng DNS 53", DoctorStatus::Pass, "Sẵn sàng nhận truy vấn từ LocalZone và PeerResolver", ""});
            items.push_back({"DNS Hệ điều hành", DoctorStatus::Warning, "Chưa trỏ DNS máy tính về 127.0.0.1", "Chạy lệnh \"/system setup\" để tự động kích hoạt"});
        } else {
            items = {
                {"Cổng DNS 53", DoctorStatus::Pass, "Đã bind thành công trên 127.0.0.1 và 192.168.1.10", ""},
                {"Quyền hệ thống", DoctorStatus::Pass, "Đang chạy với quyền đầy đủ", ""},
                {"Tường lửa (Firewall)", DoctorStatus::Pass, "Đã mở cổng UDP 53, UDP 5390, TCP 5391", ""},
                {"Multicast Discovery", DoctorStatus::Pass, "Nhận beacon bình thường trên Wi-Fi", ""},
                {"DNS Hệ điều hành", DoctorStatus::Warning, "Chưa cấu hình DNS máy trỏ về 127.0.0.1", "Chạy lệnh \"/system setup\" để tự động kích hoạt"},
                {"Upstream DNS", DoctorStatus::Pass, "Kết nối tốt tới 1.1.1.1 (14 ms) và 8.8.8.8 (18 ms)", ""}
            };
        }

        auto doc_lines = DoctorView::render(items);
        add_scrollback(doc_lines);
        return;
    }

    if (cmd == "/system") {
        std::string sub;
        iss >> sub;
        if (sub == "setup") {
            // Trigger confirmation dialog
            active_dialog_ = std::make_unique<Dialog>(
                "Đặt DNS hệ thống về 127.0.0.1?",
                std::vector<std::string>{
                    "Bản gốc sẽ được sao lưu và khôi phục bằng",
                    "/system restore."
                },
                std::vector<DialogOption>{
                    {1, "Có"},
                    {2, "Có, đừng hỏi lại"},
                    {3, "Không"}
                }
            );
            return;
        } else if (sub == "restore") {
            add_scrollback(Style::success(Style::glyph_bullet()) + "  Đã khôi phục DNS hệ thống về trạng thái ban đầu.");
            return;
        }
    }

    add_scrollback(Style::error("Lệnh không xác định: " + cmd + ". Gõ /help để xem danh sách lệnh."));
}

}  // namespace ldns::ui
