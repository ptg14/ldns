#include "ui/widgets/info_view.hpp"

#include <sstream>
#include "ui/term/style.hpp"

namespace ldns::ui {

std::vector<std::string> InfoView::render(const DomainInfo& info) {
    std::vector<std::string> lines;

    lines.push_back(Style::primary(Style::glyph_bullet()) + "  " +
                    Style::bold("Thông tin chi tiết domain: ") + Style::accent(info.domain));

    std::string owner_desc = info.owner_node + " (" + info.owner_ip;
    if (info.is_local) {
        owner_desc += " - máy của bạn)";
    } else {
        owner_desc += " - peer)";
    }

    lines.push_back(Style::glyph_indent() + "  Sở hữu: " + Style::bold(owner_desc));
    lines.push_back(Style::glyph_indent() + "  Bản ghi: " + Style::bold(info.record_type) + " → " + Style::accent(info.target));
    lines.push_back(Style::glyph_indent() + "  Thời gian sống (TTL): " + info.ttl);

    if (!info.claim_time.empty()) {
        lines.push_back(Style::glyph_indent() + "  Thời điểm đăng ký: " + info.claim_time);
    }

    std::string vote_desc = std::to_string(info.vote_count) + "/" + std::to_string(info.total_peers) + " peer đồng ý";
    if (info.vote_count == info.total_peers) {
        vote_desc += " (" + Style::success("100% đồng thuận") + ")";
    } else {
        vote_desc += " (" + Style::warning("Đang có ý kiến khác") + ")";
    }
    lines.push_back(Style::glyph_indent() + "  Đồng thuận mạng: " + vote_desc);

    if (!info.sync_status.empty()) {
        lines.push_back(Style::glyph_indent() + "  Trạng thái đồng bộ: " + Style::success(info.sync_status));
    }

    for (const auto& meta : info.metadata) {
        lines.push_back(Style::glyph_indent() + "  " + meta.first + ": " + meta.second);
    }

    return lines;
}

}  // namespace ldns::ui
