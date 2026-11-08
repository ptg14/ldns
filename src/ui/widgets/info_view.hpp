#pragma once

#include <string>
#include <utility>
#include <vector>

namespace ldns::ui {

struct DomainInfo {
    std::string domain;
    std::string owner_node;
    std::string owner_ip;
    bool is_local = false;
    std::string record_type = "A";
    std::string target;
    std::string ttl = "60s";
    int vote_count = 1;
    int total_peers = 1;
    std::string claim_time;
    std::string sync_status;
    std::vector<std::pair<std::string, std::string>> metadata;
};

class InfoView {
public:
    static std::vector<std::string> render(const DomainInfo& info);
};

}  // namespace ldns::ui
