#include "ui/widgets/doctor_view.hpp"

#include <sstream>
#include "ui/term/char_width.hpp"
#include "ui/term/style.hpp"

namespace ldns::ui {

std::vector<std::string> DoctorView::render(const std::vector<DoctorItem>& items) {
    std::vector<std::string> lines;

    lines.push_back(Style::primary(Style::glyph_bullet()) + "  " + Style::bold("Kết quả chẩn đoán Ldns:"));

    bool has_fail = false;
    bool has_warn = false;

    for (const auto& item : items) {
        std::string icon;
        switch (item.status) {
            case DoctorStatus::Pass:
                icon = Style::success(Style::glyph_check());
                break;
            case DoctorStatus::Warning:
                icon = Style::warning(Style::glyph_warn());
                has_warn = true;
                break;
            case DoctorStatus::Fail:
                icon = Style::error(Style::glyph_cross());
                has_fail = true;
                break;
        }

        std::ostringstream ss;
        ss << "  " << icon << "  ";
        std::string padded_name = item.name + ":";
        size_t cur_w = CharWidth::display_width(padded_name);
        if (cur_w < 26) {
            padded_name.append(26 - cur_w, ' ');
        }

        ss << Style::bold(padded_name) << " " << item.message;
        lines.push_back(ss.str());

        if (!item.hint.empty()) {
            std::string indent(32, ' ');
            lines.push_back(indent + Style::dim("Gợi ý: " + item.hint));
        }
    }

    lines.push_back("");
    if (has_fail) {
        lines.push_back(Style::error(Style::glyph_cross() + "  Phát hiện lỗi cấu hình. Vui lòng khắc phục theo gợi ý trên để Ldns hoạt động bình thường."));
    } else if (has_warn) {
        lines.push_back(Style::warning(Style::glyph_warn() + "  Hệ thống hoạt động nhưng có cảnh báo tối ưu. Có thể xem gợi ý trên."));
    } else {
        lines.push_back(Style::success(Style::glyph_check() + "  Không có lỗi. Toàn bộ hệ thống sẵn sàng phục vụ."));
    }

    return lines;
}

}  // namespace ldns::ui
