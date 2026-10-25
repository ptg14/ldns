#include "ui/widgets/slash_menu.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include "ui/term/style.hpp"
#include "ui/widgets/box.hpp"

namespace ldns::ui {

SlashMenu::SlashMenu() {
    commands_ = {
        {"/add", "/add <tên> [đích…]", "Tạo domain (đích mặc định self)"},
        {"/set", "/set <tên> <đích…>", "Đổi đích trỏ về của domain"},
        {"/rm", "/rm <tên>", "Xóa domain khỏi máy này"},
        {"/ls", "/ls [--mine|--all]", "Liệt kê các domain trong mạng LAN"},
        {"/info", "/info <tên>", "Xem ai đang claim, số phiếu, thứ hạng"},
        {"/import", "/import <file> [--force]", "Nhập domain hàng loạt từ file TOML/CSV"},
        {"/export", "/export [--csv]", "Xuất danh sách domain ra TOML hoặc CSV"},
        {"/resolve", "/resolve <tên> [--trace]", "Phân giải domain với cây 3 bước"},
        {"/peers", "/peers", "Xem danh sách peer: RTT, domain, phiên bản"},
        {"/peer", "/peer add|rm <ip>", "Quản lý kết nối peer thủ công"},
        {"/upstream", "/upstream list|add|set", "Quản lý danh sách DNS upstream"},
        {"/conflicts", "/conflicts", "Xem các tên miền đang bị xung đột phiếu"},
        {"/logs", "/logs [--follow]", "Xem nhật ký truy vấn DNS thời gian thực"},
        {"/status", "/status", "Tổng quan bind, quyền, peer, an toàn mạng"},
        {"/service", "/service status|install|start|stop", "Quản lý dịch vụ daemon nền hệ thống"},
        {"/network", "/network [name|key]", "Cấu hình tên mạng và khóa PSK"},
        {"/system", "/system setup|restore", "Đặt hoặc khôi phục DNS hệ thống"},
        {"/doctor", "/doctor", "Chẩn đoán cổng 53, firewall, quyền, multicast"},
        {"/help", "/help", "Xem danh sách và hướng dẫn sử dụng lệnh"},
        {"/version", "/version", "Xem thông tin phiên bản Ldns"},
        {"/exit", "/exit", "Thoát khỏi chương trình"}
    };
    filtered_ = commands_;
}

void SlashMenu::set_query(std::string_view query) {
    filtered_.clear();
    std::string q(query);
    for (const auto& cmd : commands_) {
        if (q.empty() || q == "/" || cmd.name.find(q) != std::string::npos) {
            filtered_.push_back(cmd);
        }
    }
    selected_index_ = 0;
}

void SlashMenu::move_selection_up() {
    if (!filtered_.empty()) {
        if (selected_index_ == 0) {
            selected_index_ = filtered_.size() - 1;
        } else {
            --selected_index_;
        }
    }
}

void SlashMenu::move_selection_down() {
    if (!filtered_.empty()) {
        selected_index_ = (selected_index_ + 1) % filtered_.size();
    }
}

const CommandDesc* SlashMenu::selected_command() const {
    if (selected_index_ < filtered_.size()) {
        return &filtered_[selected_index_];
    }
    return nullptr;
}

std::vector<std::string> SlashMenu::render(size_t term_width) const {
    if (filtered_.empty()) {
        return {};
    }

    size_t box_width = (term_width < 20) ? 20 : term_width;

    std::vector<std::string> lines;
    // Show maximum 7 items at a time
    size_t max_visible = 7;
    size_t start = 0;
    if (selected_index_ >= max_visible) {
        start = selected_index_ - max_visible + 1;
    }
    size_t count = std::min(filtered_.size() - start, max_visible);

    for (size_t i = 0; i < count; ++i) {
        size_t idx = start + i;
        bool is_selected = (idx == selected_index_);
        const auto& cmd = filtered_[idx];

        std::string line;
        if (is_selected) {
            line += Style::accent(Style::glyph_cursor()) + " ";
            line += Style::bold(cmd.name);
        } else {
            line += "   ";
            line += cmd.name;
        }

        // Align description column
        size_t name_len = cmd.name.size() + 3;
        if (name_len < 15) {
            line.append(15 - name_len, ' ');
        } else {
            line += " ";
        }

        if (is_selected) {
            line += cmd.description;
        } else {
            line += Style::dim(cmd.description);
        }

        lines.push_back(line);
    }

    return Box::wrap_box(lines, box_width);
}

}  // namespace ldns::ui
