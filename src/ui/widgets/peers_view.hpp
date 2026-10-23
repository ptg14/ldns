#pragma once

#include <string>
#include <vector>

namespace ldns::ui {

struct PeerNode {
    std::string hostname;
    std::string endpoint; // "192.168.1.22:5391"
    std::string rtt;      // "2.1 ms"
    int domain_count = 0;
    std::string version = "v0.1.0";
    bool is_online = true;
};

class PeersView {
public:
    static std::vector<std::string> render(const std::vector<PeerNode>& peers);
};

}  // namespace ldns::ui
