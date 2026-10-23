#pragma once

#include <string>
#include <vector>

namespace ldns::ui {

enum class DoctorStatus {
    Pass,
    Warning,
    Fail
};

struct DoctorItem {
    std::string name;
    DoctorStatus status;
    std::string message;
    std::string hint;
};

class DoctorView {
public:
    static std::vector<std::string> render(const std::vector<DoctorItem>& items);
};

}  // namespace ldns::ui
