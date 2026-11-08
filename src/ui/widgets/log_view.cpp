#include "ui/widgets/log_view.hpp"

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

std::string LogView::format_entry(const QueryLogEntry& entry) {
    std::string s_time = pad_str(Style::dim(entry.timestamp), 14);
    std::string s_ip = pad_str(entry.client_ip, 16);
    std::string s_type = pad_str(Style::bold(entry.qtype), 6);
    std::string s_domain = pad_str(Style::accent(entry.domain), 22);

    std::string s_step;
    if (entry.step.find("L:") != std::string::npos) {
        s_step = pad_str(Style::success(entry.step), 14);
    } else if (entry.step.find("P:") != std::string::npos) {
        s_step = pad_str(Style::primary(entry.step), 14);
    } else {
        s_step = pad_str(Style::dim(entry.step), 14);
    }

    std::string s_rcode;
    if (entry.rcode == "NOERROR") {
        s_rcode = pad_str(Style::success(entry.rcode), 10);
    } else if (entry.rcode == "NXDOMAIN") {
        s_rcode = pad_str(Style::warning(entry.rcode), 10);
    } else {
        s_rcode = pad_str(Style::error(entry.rcode), 10);
    }

    std::string s_lat = Style::dim(entry.latency);

    return "  " + s_time + " " + s_ip + " " + s_type + " " + s_domain + " " + s_step + " " + s_rcode + " " + s_lat;
}

std::vector<std::string> LogView::render(const std::vector<QueryLogEntry>& entries) {
    std::vector<std::string> lines;

    lines.push_back(Style::primary(Style::glyph_bullet()) + "  " +
                    Style::bold("Nhật ký truy vấn DNS") + Style::dim(" (gần nhất " + std::to_string(entries.size()) + " truy vấn):"));
    
    std::string header = "  " + pad_str("Thời gian", 14) + " " +
                         pad_str("Client IP", 16) + " " +
                         pad_str("Loại", 6) + " " +
                         pad_str("Tên domain", 22) + " " +
                         pad_str("Phân giải", 14) + " " +
                         pad_str("Kết quả", 10) + " " +
                         "Độ trễ";
    lines.push_back(Style::dim(header));
    lines.push_back(Style::dim("  ────────────────────────────────────────────────────────────────────────────────────────────"));

    for (const auto& e : entries) {
        lines.push_back(format_entry(e));
    }

    return lines;
}

}  // namespace ldns::ui
