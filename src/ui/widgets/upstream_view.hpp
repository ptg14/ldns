#pragma once

#include <string>
#include <vector>

namespace ldns::ui {

struct UpstreamServer {
    std::string ip_port;  // "1.1.1.1:53"
    std::string provider; // "Cloudflare"
    std::string rtt;      // "14 ms"
    int priority = 1;     // 1, 2, 3
    bool is_healthy = true;
};

class UpstreamView {
public:
    static std::vector<std::string> render(const std::vector<UpstreamServer>& servers);
};

}  // namespace ldns::ui
