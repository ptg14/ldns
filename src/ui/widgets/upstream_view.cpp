#include "ui/widgets/upstream_view.hpp"

#include <iomanip>
#include <sstream>
#include "ui/term/char_width.hpp"
#include "ui/term/style.hpp"

namespace ldns::ui {

static std::string pad_str(const std::string& text, size_t target_width) {
    size_t cur_w = CharWidth::display_width(text);
    if (cur_w >= target_width) return text;
    return text + std::string(target_width - cur_w, ' ');
}

std::vector<std::string> UpstreamView::render(const std::vector<UpstreamServer>& servers) {
    std::vector<std::string> lines;

    lines.push_back(Style::primary(Style::glyph_bullet()) + "  " +
                    Style::bold("Danh sách Upstream DNS Server") +
                    Style::dim(" (dự phòng bước 3 khi không tìm thấy trong LAN):"));
    lines.push_back("");

    for (const auto& s : servers) {
        std::string dot = s.is_healthy ? Style::success("●  ") : Style::error("●  ");
        std::string server_str = s.ip_port + " (" + s.provider + ")";
        server_str = pad_str(server_str, 28);
        std::string rtt_str = pad_str("RTT: " + s.rtt, 14);
        std::string prio_str = pad_str("Ưu tiên " + std::to_string(s.priority), 14);
        std::string status_str = s.is_healthy ? Style::success("Hoạt động") : Style::error("Lỗi kết nối");

        lines.push_back("    " + dot + server_str + rtt_str + prio_str + status_str);
    }

    lines.push_back("");
    lines.push_back("  " + Style::dim("Gợi ý: Dùng /upstream add <ip> để thêm DNS upstream, /upstream rm <ip> để gỡ bỏ."));

    return lines;
}

}  // namespace ldns::ui
