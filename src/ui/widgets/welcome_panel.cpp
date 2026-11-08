#include "ui/widgets/welcome_panel.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>
#include "ui/term/char_width.hpp"
#include "ui/term/style.hpp"
#include "ui/widgets/box.hpp"

namespace ldns::ui {

std::vector<std::string> WelcomePanel::render(const WelcomeInfo& info, size_t box_width) {
    if (box_width < 20) box_width = 20;

    size_t inner_width = (box_width >= 2) ? (box_width - 2) : 1;
    std::vector<std::string> lines;

    // Header line: ✻  Chào mừng đến với Ldns!
    std::string header;
    if (inner_width >= 30) {
        header = "  " + Style::accent(Style::glyph_spinner()) + "  " +
                 Style::bold("Chào mừng đến với Ldns!");
    } else {
        header = "  " + Style::accent(Style::glyph_spinner()) + "  " +
                 Style::bold("Ldns");
    }
    lines.push_back(header);
    lines.push_back("");

    // Clean node ID (extract alphanumeric hex ID, avoid broken multi-byte UTF-8 substrings)
    std::string clean_id;
    for (char c : info.node_id) {
        if (std::isxdigit(static_cast<unsigned char>(c))) {
            clean_id.push_back(c);
            if (clean_id.size() == 8) break;
        }
    }
    if (clean_id.empty()) {
        clean_id = info.node_id;
    }

    // Details rows
    if (inner_width >= 56) {
        // Calculate visual width of left column to align the second column perfectly
        size_t col1_w1 = 10 + CharWidth::display_width(info.node_name) + 1 + 1 + CharWidth::display_width(clean_id) + 1;
        size_t col1_w2 = 10 + CharWidth::display_width(info.ip_address);
        size_t max_left = std::max(col1_w1, col1_w2);
        size_t col2_pos = std::max(size_t(34), max_left + 4);
        if (col2_pos + 24 > inner_width) {
            col2_pos = (inner_width > 24) ? (inner_width - 24) : (max_left + 1);
        }

        std::ostringstream r1;
        r1 << "   node:  " << Style::bold(info.node_name) << " "
           << Style::dim("(" + clean_id + ")");
        if (col2_pos > col1_w1) {
            r1 << std::string(col2_pos - col1_w1, ' ');
        }
        r1 << "mạng: " << Style::primary(info.network_name);
        lines.push_back(r1.str());

        std::ostringstream r2;
        r2 << "   ip:    " << info.ip_address;
        if (col2_pos > col1_w2) {
            r2 << std::string(col2_pos - col1_w2, ' ');
        }
        r2 << "dns:  " << info.dns_port << "  "
           << (info.dns_active ? Style::success(Style::glyph_dot() + "  hoạt động")
                                : Style::error(Style::glyph_dot() + "  ngưng"));
        lines.push_back(r2.str());

        std::ostringstream r3;
        r3 << "   khóa:  "
           << (info.psk_enabled ? Style::success(Style::glyph_dot() + "  đã kích hoạt (AES-256-GCM)")
                                : Style::warning(Style::glyph_dot() + "  chưa đặt khóa mạng PSK"));
        lines.push_back(r3.str());
    } else if (inner_width >= 36) {
        std::ostringstream r1;
        r1 << "   node: " << Style::bold(info.node_name) << " "
           << Style::dim("(" + clean_id + ")");
        lines.push_back(r1.str());

        std::ostringstream r2;
        r2 << "   mạng: " << Style::primary(info.network_name)
           << "  ·  ip: " << info.ip_address;
        lines.push_back(r2.str());

        std::ostringstream r3;
        r3 << "   dns:  " << info.dns_port << "  "
           << (info.dns_active ? Style::success(Style::glyph_dot() + "  hoạt động")
                                : Style::error(Style::glyph_dot() + "  ngưng"));
        lines.push_back(r3.str());

        std::ostringstream r4;
        r4 << "   khóa: "
           << (info.psk_enabled ? Style::success(Style::glyph_dot() + "  đã kích hoạt")
                                : Style::warning(Style::glyph_dot() + "  chưa đặt"));
        lines.push_back(r4.str());
    } else {
        lines.push_back("   node: " + Style::bold(info.node_name));
        lines.push_back("   ip:   " + info.ip_address);
        lines.push_back("   dns:  " + info.dns_port);
        lines.push_back("   khóa: " + (info.psk_enabled ? Style::success("bật") : Style::warning("chưa")));
    }

    return Box::wrap_box(lines, box_width);
}

}  // namespace ldns::ui
