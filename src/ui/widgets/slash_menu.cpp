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
        {"/firewall", "/firewall [status|add|rm]", "Quản lý quy tắc tường lửa LAN cho Ldns"},
        {"/service", "/service status|install|start|stop", "Quản lý dịch vụ daemon nền hệ thống"},
        {"/network", "/network [name|key]", "Cấu hình tên mạng và khóa PSK"},
        {"/system", "/system setup|restore", "Đặt hoặc khôi phục DNS hệ thống"},
        {"/doctor", "/doctor", "Chẩn đoán cổng 53, firewall, quyền, multicast"},
        {"/help", "/help", "Xem danh sách và hướng dẫn sử dụng lệnh"},
        {"/version", "/version", "Xem thông tin phiên bản Ldns"},
        {"/clear", "/clear", "Xóa sạch màn hình scrollback"},
        {"/exit", "/exit", "Thoát khỏi chương trình"}
    };

    subcommands_ = {
        // /firewall
        {"status", "/firewall status", "/firewall status", "Xem trạng thái các cổng tường lửa LAN (53, 5390, 5391)"},
        {"add", "/firewall add", "/firewall add", "Tự động thêm quy tắc mở các cổng tường lửa cho Ldns"},
        {"remove", "/firewall remove", "/firewall remove", "Gỡ bỏ các quy tắc tường lửa của Ldns"},

        // /network
        {"name", "/network name", "/network name <tên>", "Đổi tên cụm mạng LAN sang tên mới"},
        {"key generate", "/network key generate", "/network key generate", "Sinh tệp khóa ngẫu nhiên 256-bit PSK an toàn"},
        {"key set", "/network key set", "/network key set", "Kích hoạt mã hóa khóa mạng PSK AES-256-GCM"},
        {"key clear", "/network key clear", "/network key clear", "Xóa khóa mạng PSK, đưa cụm mạng về chế độ mở"},
        {"key status", "/network key status", "/network key status", "Xem trạng thái bảo mật của khóa mạng PSK"},

        // /service
        {"status", "/service status", "/service status", "Xem trạng thái dịch vụ nền (LdnsService / ldns.service)"},
        {"install", "/service install", "/service install", "Hộp thoại cài đặt dịch vụ tự khởi động cùng hệ thống"},
        {"start", "/service start", "/service start", "Bắt đầu chạy dịch vụ daemon nền"},
        {"stop", "/service stop", "/service stop", "Dừng dịch vụ daemon nền"},
        {"uninstall", "/service uninstall", "/service uninstall", "Gỡ bỏ dịch vụ daemon nền khỏi hệ thống"},

        // /system
        {"status", "/system status", "/system status", "Xem trạng thái cấu hình DNS hệ điều hành và metadata"},
        {"setup", "/system setup", "/system setup", "Hộp thoại cấu hình trỏ DNS máy tính về 127.0.0.1"},
        {"restore", "/system restore", "/system restore", "Khôi phục DNS hệ thống về cấu hình gốc ban đầu"},

        // /peer
        {"add", "/peer add", "/peer add <ip:5391>", "Thêm kết nối thủ công tới peer LAN"},
        {"rm", "/peer rm", "/peer rm <ip>", "Ngắt kết nối và gỡ bỏ peer thủ công"},
        {"forget", "/peer forget", "/peer forget <node>", "Quên thông tin nhận diện node peer"},

        // /upstream
        {"list", "/upstream list", "/upstream list", "Xem danh sách DNS upstream và độ trễ RTT"},
        {"add", "/upstream add", "/upstream add <ip>", "Thêm DNS upstream server mới"},
        {"rm", "/upstream rm", "/upstream rm <ip>", "Gỡ bỏ DNS upstream server khỏi danh sách"},

        // /logs
        {"--follow", "/logs --follow", "/logs --follow", "Theo dõi trực tiếp luồng truy vấn DNS thời gian thực"},

        // /resolve
        {"--trace", "/resolve --trace", "/resolve <tên> --trace", "Phân giải tên miền kèm sơ đồ cây 3 bước"},

        // /ls
        {"--mine", "/ls --mine", "/ls --mine", "Chỉ hiển thị các domain do node này tạo"},
        {"--all", "/ls --all", "/ls --all", "Hiển thị toàn bộ domain của tất cả các node trong LAN"},
        {"--type A", "/ls --type A", "/ls --type A", "Lọc danh sách domain theo loại bản ghi A"},
        {"--type AAAA", "/ls --type AAAA", "/ls --type AAAA", "Lọc danh sách domain theo loại bản ghi AAAA"},
        {"--type CNAME", "/ls --type CNAME", "/ls --type CNAME", "Lọc danh sách domain theo loại bản ghi CNAME"},
        {"--type TXT", "/ls --type TXT", "/ls --type TXT", "Lọc danh sách domain theo loại bản ghi TXT"},

        // /conflicts
        {"clear", "/conflicts clear", "/conflicts clear", "Xóa sạch các xung đột mẫu đang lưu"},

        // /import
        {"--force", "/import --force", "/import <file> --force", "Ghi đè domain khi phát hiện xung đột lúc import"},

        // /export
        {"--csv", "/export --csv", "/export --csv", "Xuất danh sách domain ra định dạng file CSV"},
        {"--out", "/export --out", "/export --out <file>", "Xuất danh sách domain ra file chỉ định"},

        // /help
        {"add", "/help add", "/help add", "Xem hướng dẫn chi tiết tham số và ví dụ /add"},
        {"set", "/help set", "/help set", "Xem hướng dẫn chi tiết tham số và ví dụ /set"},
        {"rm", "/help rm", "/help rm", "Xem hướng dẫn chi tiết tham số và ví dụ /rm"},
        {"network", "/help network", "/help network", "Xem hướng dẫn chi tiết mạng và khóa PSK"},
        {"service", "/help service", "/help service", "Xem hướng dẫn chi tiết dịch vụ nền"},
        {"system", "/help system", "/help system", "Xem hướng dẫn chi tiết DNS hệ thống"},
        {"firewall", "/help firewall", "/help firewall", "Xem hướng dẫn chi tiết tường lửa LAN"},
        {"resolve", "/help resolve", "/help resolve", "Xem hướng dẫn chi tiết phân giải và trace"},
        {"peer", "/help peer", "/help peer", "Xem hướng dẫn chi tiết quản lý peer P2P"}
    };

    filtered_ = commands_;
}

void SlashMenu::set_query(std::string_view query) {
    filtered_.clear();
    std::string q(query);

    // Case 1: No space in query -> top-level command search (e.g. "/", "/s", "/fire")
    size_t first_space = q.find(' ');
    if (first_space == std::string::npos) {
        for (const auto& cmd : commands_) {
            if (q.empty() || q == "/" || cmd.name.find(q) != std::string::npos) {
                filtered_.push_back(cmd);
            }
        }
        selected_index_ = 0;
        return;
    }

    // Case 2: Query has space -> subcommand search!
    std::string base_cmd = q.substr(0, first_space);
    size_t sub_start = q.find_first_not_of(' ', first_space);
    std::string sub_query = (sub_start != std::string::npos) ? q.substr(sub_start) : "";

    for (const auto& sub : subcommands_) {
        if (sub.completion.rfind(base_cmd, 0) == 0) {
            if (sub_query.empty()) {
                filtered_.push_back(sub);
            } else {
                if (sub.name.find(sub_query) != std::string::npos ||
                    sub.completion.find(q) != std::string::npos) {
                    filtered_.push_back(sub);
                }
            }
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
    if (filtered_.empty()) return nullptr;
    return &filtered_[selected_index_];
}

std::vector<std::string> SlashMenu::render(size_t box_width) const {
    if (filtered_.empty()) return {};

    std::vector<std::string> lines;
    // Header
    lines.push_back("  " + Style::dim("Lệnh khả dụng:") + " " +
                    Style::accent(std::to_string(filtered_.size())) +
                    Style::dim(" gợi ý  (↑/↓ chọn · Tab hoàn tất)"));
    lines.push_back("");

    // Limit to max 6 visible items
    size_t max_visible = 6;
    size_t start_idx = 0;
    if (filtered_.size() > max_visible && selected_index_ >= max_visible) {
        start_idx = selected_index_ - max_visible + 1;
    }
    size_t end_idx = std::min(start_idx + max_visible, filtered_.size());

    for (size_t i = start_idx; i < end_idx; ++i) {
        const auto& item = filtered_[i];
        bool is_selected = (i == selected_index_);

        std::string prefix = is_selected ? (Style::accent("  ❯ ") + Style::bold(item.name))
                                         : ("    " + Style::bold(item.name));

        // Pad name to 14 chars
        if (item.name.length() < 14) {
            prefix.append(14 - item.name.length(), ' ');
        }

        std::string desc = "  " + Style::dim(item.description);
        lines.push_back(prefix + desc);
    }

    if (filtered_.size() > max_visible) {
        lines.push_back("  " + Style::dim("... (" + std::to_string(filtered_.size() - max_visible) + " lệnh khác)"));
    }

    return Box::wrap_box(lines, box_width);
}

}  // namespace ldns::ui
