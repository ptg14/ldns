#pragma once

#include <string>
#include <vector>

namespace ldns::ui {

struct NetworkInfo {
    std::string network_name = "default";
    std::string discovery_endpoint = "239.76.68.83:5390 (UDP Multicast)";
    std::string sync_port = "5391 (TCP Full-mesh)";
    bool psk_enabled = false;
    std::string psk_cipher = "AES-256-GCM";
    int active_peers = 0;
};

class NetworkView {
public:
    static std::vector<std::string> render(const NetworkInfo& info);
};

}  // namespace ldns::ui
