#pragma once

#include <string>
#include <vector>

namespace ldns::ui {

struct WelcomeInfo {
    std::string node_name = "local-node";
    std::string node_id = "a3f9e1";
    std::string network_name = "default";
    std::string ip_address = "127.0.0.1";
    std::string dns_port = ":53";
    bool dns_active = true;
    bool psk_enabled = false;
};

class WelcomePanel {
public:
    static std::vector<std::string> render(const WelcomeInfo& info, size_t term_width);
};

}  // namespace ldns::ui
