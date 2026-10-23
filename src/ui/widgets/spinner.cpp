#include "ui/widgets/spinner.hpp"

#include <iomanip>
#include <sstream>
#include "ui/term/style.hpp"

namespace ldns::ui {

Spinner::Spinner(std::string action)
    : action_(std::move(action)), start_time_(std::chrono::steady_clock::now()) {
    if (Style::ascii_fallback()) {
        glyphs_ = {"*", "+", "x", "."};
    } else {
        glyphs_ = {"✻", "✼", "✽", "✹"};
    }
}

void Spinner::update() {
    frame_index_ = (frame_index_ + 1) % glyphs_.size();
}

void Spinner::set_action(std::string action) {
    action_ = std::move(action);
}

std::string Spinner::render() const {
    auto now = std::chrono::steady_clock::now();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time_).count();
    double sec = static_cast<double>(elapsed_ms) / 1000.0;

    std::ostringstream ss;
    ss << Style::accent(glyphs_[frame_index_]) << " " << action_ << " "
       << Style::dim("(") << std::fixed << std::setprecision(1) << sec << "s"
       << Style::dim(" · esc để ngắt)");

    return ss.str();
}

}  // namespace ldns::ui
