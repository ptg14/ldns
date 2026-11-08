#pragma once

#include <chrono>
#include <string>
#include <vector>

namespace ldns::ui {

class Spinner {
public:
    Spinner(std::string action = "Đang xử lý…");

    void update();
    void set_action(std::string action);
    std::string render() const;

private:
    std::string action_;
    size_t frame_index_ = 0;
    std::chrono::steady_clock::time_point start_time_;
    std::vector<std::string> glyphs_;
};

}  // namespace ldns::ui
