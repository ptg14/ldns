#include "ui/widgets/network_view.hpp"

#include <sstream>
#include "ui/term/style.hpp"

namespace ldns::ui {

std::vector<std::string> NetworkView::render(const NetworkInfo& info) {
    std::vector<std::string> lines;

    lines.push_back(Style::primary(Style::glyph_bullet()) + "  " +
                    Style::bold("Cấu hình Cụm Mạng Ldns (LAN Cluster):"));
    lines.push_back(Style::glyph_indent() + "  Tên mạng (Cluster):   " + Style::accent("\"" + info.network_name + "\""));
    lines.push_back(Style::glyph_indent() + "  Cổng Discovery:      " + info.discovery_endpoint);
    lines.push_back(Style::glyph_indent() + "  Cổng Đồng bộ (Sync):  " + info.sync_port);
    lines.push_back(Style::glyph_indent() + "  Số peer cùng mạng:   " + std::to_string(info.active_peers) + " node");

    if (info.psk_enabled) {
        lines.push_back(Style::glyph_indent() + "  Khóa mạng PSK:       " + Style::success("Đã bật (" + info.psk_cipher + " - Mạng được mã hóa bảo mật)"));
    } else {
        lines.push_back(Style::glyph_indent() + "  Khóa mạng PSK:       " + Style::warning("Chưa thiết lập (Mạng mở không mã hóa)"));
        lines.push_back("  " + Style::warning("⚠  Cảnh báo: Bất kỳ máy nào trong LAN đều có thể tham gia cụm này."));
        lines.push_back("  " + Style::dim("Gợi ý: Dùng \"/network key set <khóa>\" để kích hoạt mã hóa toàn cụm."));
    }

    return lines;
}

}  // namespace ldns::ui
