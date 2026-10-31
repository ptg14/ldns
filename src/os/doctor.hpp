#pragma once

#include <string>
#include <vector>

#include "core/core_api.hpp"

namespace ldns::os {

struct DiagnosticItem {
    std::string name;
    bool passed{false};
    std::string detail;
    std::string fix_hint;
};

struct DoctorReport {
    std::vector<DiagnosticItem> items;
    bool all_passed{true};
    std::size_t passed_count{0};
    std::size_t failed_count{0};
};

class Doctor {
public:
    static DoctorReport run_diagnostics(CoreApi& api);
};

} // namespace ldns::os
