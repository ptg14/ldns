#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/dns/name.hpp"
#include "core/registry/registry.hpp"
#include "core/util/id.hpp"

namespace ldns::resolver {

// Canonicalizes RRset data per protocol.md §2.4:
// 1. Sort bytes lexicographically ascending.
// 2. Deduplicate identical elements.
// 3. TTL is ignored during comparison.
std::vector<std::vector<std::uint8_t>> canonicalize_rrset_data(
    std::vector<std::vector<std::uint8_t>> data);

struct PeerCandidate {
    NodeId node_id{};
    std::uint64_t claim_created_at{0};
    std::uint64_t rtt_ms{0};
    bool online{true};

    bool operator==(const PeerCandidate& other) const = default;
};

struct PeerGroup {
    std::uint16_t type{1};
    std::uint32_t ttl{60};
    std::vector<std::vector<std::uint8_t>> canonical_data;
    std::size_t vote_count{0};
    std::uint64_t oldest_claim_created{0};
    NodeId lowest_node_id{};
    std::vector<PeerCandidate> candidates;

    bool operator==(const PeerGroup& other) const = default;
};

struct ConflictInfo {
    dns::Name name;
    std::uint16_t type{1};
    std::vector<PeerGroup> groups;
    bool local_divergence{false};
};

struct RankingResult {
    std::vector<PeerGroup> groups;
    std::optional<ConflictInfo> conflict;
};

// Pure deterministic ranking function (D10, SPEC §6.2.2c, protocol §8.4):
// Evaluates snapshot claims for (qname, qtype):
// 1. Exact match first, wildcard second; CNAME fallback if question is not CNAME.
// 2. Groups by (type, canonical_data).
// 3. Sorts groups:
//    - vote_count descending
//    - oldest_claim_created ascending
//    - lowest_node_id ascending (lexicographical byte order)
// 4. Sorts candidates within each group:
//    - rtt_ms ascending
//    - node_id ascending
// 5. Detects conflicts when groups.size() > 1 or diverges from local_canonical_data.
RankingResult rank_peer_claims(
    const RegistrySnapshot& snapshot,
    const dns::Name& qname,
    std::uint16_t qtype,
    bool online_only = true,
    const std::unordered_map<NodeId, std::uint64_t>& rtt_map = {},
    const std::vector<std::vector<std::uint8_t>>* local_canonical_data = nullptr);

}  // namespace ldns::resolver
