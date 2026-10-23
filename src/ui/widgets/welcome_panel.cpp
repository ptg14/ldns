#include "ui/widgets/welcome_panel.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>
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

    // Quick tips
    if (inner_width >= 62) {
        std::string tips = "   " + Style::primary("/help") + Style::dim(" để xem lệnh · ") +
                           Style::primary("/add app.lan") + Style::dim(" để tạo domain đầu tiên");
        lines.push_back(tips);
    } else if (inner_width >= 36) {
        lines.push_back("   " + Style::primary("/help") + Style::dim(" để xem danh sách lệnh"));
        lines.push_back("   " + Style::primary("/add app.lan") + Style::dim(" tạo domain đầu tiên"));
    } else {
        lines.push_back("   " + Style::primary("/help") + Style::dim(" xem lệnh"));
    }
    lines.push_back("");

    // Details rows
    if (inner_width >= 56) {
        std::ostringstream r1;
        r1 << "   node:  " << Style::bold(info.node_name) << " "
           << Style::dim("(" + info.node_id.substr(0, 6) + "…)")
           << "       mạng: " << Style::primary(info.network_name);
        lines.push_back(r1.str());

        std::ostringstream r2;
        r2 << "   ip:    " << info.ip_address
           << "             dns:  " << info.dns_port << "  "
           << (info.dns_active ? Style::success(Style::glyph_dot() + "  hoạt động")
                               : Style::error(Style::glyph_dot() + "  ngưng"));
        lines.push_back(r2.str());
    } else if (inner_width >= 36) {
        std::ostringstream r1;
        r1 << "   node: " << Style::bold(info.node_name) << " "
           << Style::dim("(" + info.node_id.substr(0, 6) + "…)");
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
    } else {
        lines.push_back("   node: " + Style::bold(info.node_name));
        lines.push_back("   ip:   " + info.ip_address);
        lines.push_back("   dns:  " + info.dns_port);
    }

    return Box::wrap_box(lines, box_width);
}

}  // namespace ldns::ui
