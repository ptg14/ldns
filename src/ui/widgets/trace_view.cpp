#include "ui/widgets/trace_view.hpp"

#include <sstream>
#include "ui/term/style.hpp"

namespace ldns::ui {

std::vector<std::string> TraceView::render(const TraceResult& trace) {
    std::vector<std::string> lines;

    // Header line: ⏺  Kết quả: 192.168.1.22 (A) · TTL 30s
    std::ostringstream h_ss;
    h_ss << Style::success(Style::glyph_bullet()) << "  "
         << Style::bold("Kết quả:") << " " << Style::primary(trace.final_answer)
         << " (" << trace.record_type << ") " << Style::dim("·") << " TTL " << trace.ttl;
    lines.push_back(h_ss.str());

    // Indented detail: ⎿  Quá trình phân giải (tổng thời gian: 18.4 ms):
    std::ostringstream d_ss;
    d_ss << Style::glyph_indent() << "  Quá trình phân giải ("
         << Style::bold("tổng thời gian: " + trace.total_duration) << "):";
    lines.push_back(d_ss.str());

    // Tree steps
    for (size_t i = 0; i < trace.steps.size(); ++i) {
        const auto& step = trace.steps[i];
        bool is_last_step = (i + 1 == trace.steps.size());
        std::string branch = is_last_step ? "    └─ " : "    ├─ ";

        std::ostringstream s_ss;
        s_ss << Style::dim(branch) << "[" << Style::bold(step.step_name) << "] "
             << step.details;
        if (!step.latency.empty()) {
            s_ss << " " << Style::dim("[" + step.latency + "]");
        }
        lines.push_back(s_ss.str());

        std::string sub_prefix = is_last_step ? "        " : "    │   ";
        for (size_t j = 0; j < step.sub_details.size(); ++j) {
            bool is_last_sub = (j + 1 == step.sub_details.size());
            std::string sub_branch = is_last_sub ? "└─ " : "├─ ";
            lines.push_back(Style::dim(sub_prefix + sub_branch) + step.sub_details[j]);
        }
    }

    return lines;
}

}  // namespace ldns::ui
