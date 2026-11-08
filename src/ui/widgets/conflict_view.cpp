#include "ui/widgets/conflict_view.hpp"

#include <sstream>
#include "ui/term/style.hpp"

namespace ldns::ui {

std::vector<std::string> ConflictView::render(const std::vector<ConflictDomain>& conflicts) {
    std::vector<std::string> lines;

    if (conflicts.empty()) {
        lines.push_back(Style::success(Style::glyph_check()) + "  " +
                        Style::bold("Không có xung đột tên miền nào trên mạng LAN."));
        lines.push_back("   Mọi bản ghi đều đạt đồng thuận tuyệt đối giữa các peer.");
        return lines;
    }

    lines.push_back(Style::warning(Style::glyph_warn()) + "  " +
                    Style::bold("Phát hiện " + std::to_string(conflicts.size()) + " domain có xung đột bản ghi trên mạng LAN:"));
    lines.push_back("");

    for (const auto& item : conflicts) {
        lines.push_back("  " + Style::bold("Domain: ") + Style::accent(item.domain) +
                        Style::dim(" (Loại: " + item.record_type + ")"));
        lines.push_back("  ──────────────────────────────────────────────────────────────────");

        for (const auto& grp : item.groups) {
            std::string title = "Nhóm " + std::to_string(grp.group_id);
            if (grp.is_winner) {
                title += " (" + Style::success("Chiến thắng") + " - " + std::to_string(grp.vote_count) + " phiếu):";
            } else {
                title += " (" + Style::error("Thua") + " - " + std::to_string(grp.vote_count) + " phiếu):";
            }
            lines.push_back("  " + title);

            std::string claimants_str;
            for (size_t i = 0; i < grp.claimants.size(); ++i) {
                claimants_str += grp.claimants[i];
                if (i + 1 < grp.claimants.size()) claimants_str += ", ";
            }
            lines.push_back("    Đích: " + Style::bold(grp.target_endpoint) + " (" + claimants_str + ")");
            if (grp.is_winner) {
                lines.push_back("    Trạng thái: " + Style::success("Đang được phân giải cho toàn mạng"));
            }
        }

        lines.push_back("  ──────────────────────────────────────────────────────────────────");
        if (!item.resolution_hint.empty()) {
            lines.push_back("  " + Style::dim("Gợi ý: ") + item.resolution_hint);
        }
        lines.push_back("");
    }

    return lines;
}

}  // namespace ldns::ui
