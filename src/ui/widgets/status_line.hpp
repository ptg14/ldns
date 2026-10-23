#pragma once

#include <cstddef>
#include <string>

namespace ldns::ui {

struct StatusStats {
    size_t peer_count = 0;
    size_t domain_count = 0;
    size_t conflict_count = 0;
    bool online = true;
};

class StatusLine {
public:
    static std::string render(const StatusStats& stats, std::string_view hint, size_t term_width);
};

}  // namespace ldns::ui
