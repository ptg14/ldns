#pragma once

#include <string>
#include <vector>

namespace ldns::ui {

struct TraceStep {
    std::string step_name; // e.g. "1. LocalZone"
    std::string details;   // e.g. "myapp.lan: Không tìm thấy (NXDOMAIN cục bộ)"
    std::string latency;   // e.g. "0.1 ms"
    std::vector<std::string> sub_details;
    bool is_last = false;
};

struct TraceResult {
    std::string target_domain;
    std::string final_answer;
    std::string record_type = "A";
    std::string ttl = "30s";
    std::string total_duration = "18.4 ms";
    std::vector<TraceStep> steps;
};

class TraceView {
public:
    static std::vector<std::string> render(const TraceResult& trace);
};

}  // namespace ldns::ui
