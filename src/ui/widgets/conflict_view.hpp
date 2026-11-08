#pragma once

#include <string>
#include <vector>

namespace ldns::ui {

struct ConflictVoteGroup {
    int group_id;
    int vote_count;
    std::string target_endpoint;
    std::vector<std::string> claimants;
    bool is_winner;
};

struct ConflictDomain {
    std::string domain;
    std::string record_type = "A";
    std::vector<ConflictVoteGroup> groups;
    std::string resolution_hint;
};

class ConflictView {
public:
    static std::vector<std::string> render(const std::vector<ConflictDomain>& conflicts);
};

}  // namespace ldns::ui
