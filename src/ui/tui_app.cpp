#include "ui/tui_app.hpp"

#include <chrono>
#include <iostream>
#include <sstream>
#include <thread>
#include "ui/term/input_event.hpp"
#include "ui/term/style.hpp"
#include "ui/widgets/doctor_view.hpp"
#include "ui/widgets/spinner.hpp"
#include "ui/widgets/trace_view.hpp"
#include "ui/widgets/welcome_panel.hpp"

namespace ldns::ui {

TuiApp::TuiApp() : term_(Terminal::instance()), renderer_(term_) {
    stats_.peer_count = 3;
    stats_.domain_count = 14;
    stats_.conflict_count = 0;
    stats_.online = true;

    my_domains_ = {
        {"app.lan", "192.168.1.10 (self)"},
        {"db.lan", "192.168.1.10 (self)"}
    };

    welcome_info_.node_name = "thang-pc";
    welcome_info_.node_id = "a3f9e1c2";
    welcome_info_.network_name = "default";
    welcome_info_.ip_address = "192.168.1.10";
    welcome_info_.dns_port = ":53";
    welcome_info_.dns_active = true;
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
            } else if (ev.type == KeyType::Tab || ev.type == KeyType::Enter) {
                const auto* cmd = slash_menu_.selected_command();
                if (cmd) {
                    input_box_.set_text(cmd->name + " ");
                }
                show_slash_menu_ = false;
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
                if (!cur_text.empty() && cur_text[0] == '/') {
                    show_slash_menu_ = true;
                    slash_menu_.set_query(cur_text);
                } else {
                    show_slash_menu_ = false;
                }
                break;
            }
            case KeyType::Backspace:
                input_box_.backspace();
                if (input_box_.is_search_mode()) {
                    break;
                }
                if (input_box_.text().empty() || input_box_.text()[0] != '/') {
                    show_slash_menu_ = false;
                } else {
                    slash_menu_.set_query(input_box_.text());
                }
                break;
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
        lines.push_back("  " + Style::bold("/add <tên> [đích]") + "        Tạo domain (mặc định IP self)");
        lines.push_back("  " + Style::bold("/ls") + "                       Liệt kê toàn bộ domain mạng LAN");
        lines.push_back("  " + Style::bold("/rm <tên>") + "                 Xóa domain");
        lines.push_back("  " + Style::bold("/info <tên>") + "               Chi tiết số phiếu và thứ hạng");
        lines.push_back("  " + Style::bold("/resolve <tên> [--trace]") + "   Phân giải thử nghiệm kèm sơ đồ cây");
        lines.push_back("  " + Style::bold("/peers") + "                    Xem danh sách các máy peer");
        lines.push_back("  " + Style::bold("/conflicts") + "                Xem các tên miền bị tranh chấp");
        lines.push_back("  " + Style::bold("/doctor") + "                   Chẩn đoán sức khỏe hệ thống");
        lines.push_back("  " + Style::bold("/status") + "                   Tổng quan hệ thống và bảo mật");
        lines.push_back("  " + Style::bold("/system setup|restore") + "     Cấu hình hoặc hoàn tác DNS HĐH");
        lines.push_back("  " + Style::bold("/exit") + "                     Thoát khỏi chương trình");
        add_scrollback(lines);
        return;
    }

    if (cmd == "/status" || cmd == "status") {
        std::vector<std::string> lines;
        lines.push_back(Style::primary(Style::glyph_bullet()) + "  " + Style::bold("Tổng quan Ldns") + " (node: thang-pc · a3f9e1)");
        lines.push_back(Style::glyph_indent() + "  Phiên bản: 0.1.0 · Cổng discovery: 5390 · Cổng sync: 5391");
        lines.push_back(Style::glyph_indent() + "  " + Style::warning("Cảnh báo: Chưa đặt khóa mạng PSK (/network key set để bật mã hóa)"));
        lines.push_back("");
        lines.push_back("  " + Style::bold("Dịch vụ DNS:"));
        lines.push_back("    " + Style::success(Style::glyph_dot()) + "  127.0.0.1:53       Hoạt động (4,821 truy vấn · avg 0.8ms)");
        lines.push_back("    " + Style::success(Style::glyph_dot()) + "  192.168.1.10:53    Hoạt động (LAN interface: Wi-Fi)");
        lines.push_back("");
        lines.push_back("  " + Style::bold("Kết nối Peer (" + std::to_string(stats_.peer_count) + " peer online):"));
        lines.push_back("    minh-laptop (192.168.1.22)   RTT 2.1ms   8 domain   v0.1.0");
        lines.push_back("    nam-desktop (192.168.1.45)   RTT 1.4ms  15 domain   v0.1.0");
        lines.push_back("    pi-server   (192.168.1.100)  RTT 0.9ms   3 domain   v0.1.0");
        add_scrollback(lines);
        return;
    }

    if (cmd == "/add") {
        std::string name;
        std::string target;
        iss >> name >> target;
        if (name.empty()) {
            add_scrollback(Style::error("Lỗi: Thiếu tên domain. Cú pháp: /add <tên> [đích]"));
            return;
        }
        if (target.empty()) {
            target = "192.168.1.10 (self)";
        }
        my_domains_.push_back({name, target});
        stats_.domain_count++;

        add_scrollback(Style::success(Style::glyph_bullet()) + "  Đã thêm " + Style::bold(name) + " → " + target);
        add_scrollback(Style::glyph_indent() + "  TTL 60s · đã đồng bộ tới " + std::to_string(stats_.peer_count) + " peer trong 32 ms");
        return;
    }

    if (cmd == "/ls") {
        std::vector<std::string> lines;
        lines.push_back(Style::primary(Style::glyph_bullet()) + "  Danh sách domain (" + std::to_string(my_domains_.size()) + " cục bộ):");
        for (const auto& d : my_domains_) {
            lines.push_back("  " + Style::bold(d.first) + "  →  " + d.second);
        }
        add_scrollback(lines);
        return;
    }

    if (cmd == "/rm") {
        std::string name;
        iss >> name;
        if (name.empty()) {
            add_scrollback(Style::error("Lỗi: Thiếu tên domain. Cú pháp: /rm <tên>"));
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
        } else {
            add_scrollback(Style::error("Không tìm thấy domain " + name + " thuộc sở hữu của bạn."));
        }
        return;
    }

    if (cmd == "/resolve") {
        std::string name;
        std::string flag;
        iss >> name >> flag;
        if (name.empty()) {
            add_scrollback(Style::error("Lỗi: Thiếu tên cần phân giải. Cú pháp: /resolve <tên> [--trace]"));
            return;
        }

        // Show spinner animation to demonstrate async task UX
        Spinner spinner("Đang phân giải " + name + "…");
        for (int i = 0; i < 5; ++i) {
            spinner.update();
            std::vector<std::string> sp_lines = {spinner.render()};
            renderer_.render_dynamic_zone(sp_lines);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        renderer_.clear_dynamic_zone();

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

    if (cmd == "/doctor") {
        Spinner spinner("Đang chẩn đoán hệ thống…");
        for (int i = 0; i < 6; ++i) {
            spinner.update();
            std::vector<std::string> sp_lines = {spinner.render()};
            renderer_.render_dynamic_zone(sp_lines);
            std::this_thread::sleep_for(std::chrono::milliseconds(90));
        }
        renderer_.clear_dynamic_zone();

        std::vector<DoctorItem> items = {
            {"Cổng DNS 53", DoctorStatus::Pass, "Đã bind thành công trên 127.0.0.1 và 192.168.1.10", ""},
            {"Quyền hệ thống", DoctorStatus::Pass, "Đang chạy với quyền đầy đủ", ""},
            {"Tường lửa (Firewall)", DoctorStatus::Pass, "Đã mở cổng UDP 53, UDP 5390, TCP 5391", ""},
            {"Multicast Discovery", DoctorStatus::Pass, "Nhận beacon bình thường trên Wi-Fi", ""},
            {"DNS Hệ điều hành", DoctorStatus::Warning, "Chưa cấu hình DNS máy trỏ về 127.0.0.1", "Chạy lệnh \"/system setup\" để tự động kích hoạt"},
            {"Upstream DNS", DoctorStatus::Pass, "Kết nối tốt tới 1.1.1.1 (14 ms) và 8.8.8.8 (18 ms)", ""}
        };

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

    if (cmd == "/peers") {
        std::vector<std::string> lines;
        lines.push_back(Style::primary(Style::glyph_bullet()) + "  Danh sách các peer trong mạng LAN:");
        lines.push_back("  " + Style::bold("minh-laptop") + "  192.168.1.22:5391   RTT: 2.1ms   8 domain    ●  online");
        lines.push_back("  " + Style::bold("nam-desktop") + "  192.168.1.45:5391   RTT: 1.4ms  15 domain    ●  online");
        lines.push_back("  " + Style::bold("pi-server") + "    192.168.1.100:5391  RTT: 0.9ms   3 domain    ●  online");
        add_scrollback(lines);
        return;
    }

    if (cmd == "/conflicts") {
        std::vector<std::string> lines;
        lines.push_back(Style::success(Style::glyph_bullet()) + "  Không có xung đột tên miền nào trên mạng LAN hiện tại.");
        add_scrollback(lines);
        return;
    }

    add_scrollback(Style::error("Lệnh không xác định: " + cmd + ". Gõ /help để xem danh sách lệnh."));
}

}  // namespace ldns::ui
