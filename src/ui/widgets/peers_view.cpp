#include "ui/widgets/peers_view.hpp"

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

std::vector<std::string> PeersView::render(const std::vector<PeerNode>& peers) {
    std::vector<std::string> lines;

    lines.push_back(Style::primary(Style::glyph_bullet()) + "  " +
                    Style::bold("Danh sách kết nối Peer LAN") +
                    Style::dim(" (" + std::to_string(peers.size()) + " node trực tuyến):"));
    lines.push_back("");

    std::string header = "  " + pad_str("Tên Node", 18) + " " +
                         pad_str("Địa chỉ Sync (TCP)", 22) + " " +
                         pad_str("Độ trễ RTT", 14) + " " +
                         pad_str("Domain", 12) + " " +
                         pad_str("Phiên bản", 12) + " " +
                         "Trạng thái";
    lines.push_back(Style::dim(header));
    lines.push_back(Style::dim("  ────────────────────────────────────────────────────────────────────────────────────────"));

    for (const auto& peer : peers) {
        std::string s_host = pad_str(Style::bold(peer.hostname), 18);
        std::string s_end = pad_str(peer.endpoint, 22);
        std::string s_rtt = pad_str(Style::accent(peer.rtt), 14);
        std::string s_dom = pad_str(std::to_string(peer.domain_count) + " domain", 12);
        std::string s_ver = pad_str(Style::dim(peer.version), 12);
        std::string s_status = peer.is_online ? Style::success("●  online") : Style::error("●  offline");

        lines.push_back("  " + s_host + " " + s_end + " " + s_rtt + " " + s_dom + " " + s_ver + " " + s_status);
    }

    lines.push_back("");
    lines.push_back("  " + Style::dim("Gợi ý: Dùng /peer add <ip:cổng> để nối thêm peer thủ công, /peer rm <ip> để ngắt kết nối."));

    return lines;
}

}  // namespace ldns::ui
