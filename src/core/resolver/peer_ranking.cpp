#include "core/resolver/peer_ranking.hpp"

#include <algorithm>
#include <map>
#include <utility>

#include "core/dns/message.hpp"

namespace ldns::resolver {

std::vector<std::vector<std::uint8_t>> canonicalize_rrset_data(
    std::vector<std::vector<std::uint8_t>> data) {
    std::sort(data.begin(), data.end());
    data.erase(std::unique(data.begin(), data.end()), data.end());
    return data;
}

namespace {

const OwnerRecord* find_owner_answer(const OwnerState& state, const dns::Name& qname, std::uint16_t qtype) {
    auto find_record = [&](const dns::Name& n, std::uint16_t t) -> const OwnerRecord* {
        const auto it = state.records.find(std::make_pair(n.to_string(), t));
        if (it != state.records.end()) return &it->second;
        return nullptr;
    };

    // 1. Exact match for (qname, qtype)
    if (const auto* rec = find_record(qname, qtype)) return rec;

    // 2. Wildcard match for (*.parent, qtype)
    for (dns::Name ancestor = qname.parent(); !ancestor.is_root(); ancestor = ancestor.parent()) {
        auto wildcard = dns::Name::make_wildcard(ancestor);
        if (!wildcard) break;
        if (const auto* rec = find_record(wildcard.value(), qtype)) return rec;
    }

    // 3. If qtype != CNAME, check for CNAME fallback (SPEC §6.2.2a, §8.2)
    if (qtype != static_cast<std::uint16_t>(dns::RrType::CNAME)) {
        if (const auto* rec = find_record(qname, static_cast<std::uint16_t>(dns::RrType::CNAME))) {
            return rec;
        }
        for (dns::Name ancestor = qname.parent(); !ancestor.is_root(); ancestor = ancestor.parent()) {
            auto wildcard = dns::Name::make_wildcard(ancestor);
            if (!wildcard) break;
            if (const auto* rec = find_record(wildcard.value(), static_cast<std::uint16_t>(dns::RrType::CNAME))) {
                return rec;
            }
        }
    }

    return nullptr;
}

}  // namespace

RankingResult rank_peer_claims(
    const RegistrySnapshot& snapshot,
    const dns::Name& qname,
    std::uint16_t qtype,
    bool online_only,
    const std::unordered_map<NodeId, std::uint64_t>& rtt_map,
    const std::vector<std::vector<std::uint8_t>>* local_canonical_data) {

    RankingResult result;

    using GroupKey = std::pair<std::uint16_t, std::vector<std::vector<std::uint8_t>>>;
    std::map<GroupKey, PeerGroup> group_map;

    for (const auto& [owner_id, owner_state] : snapshot.owners()) {
        if (online_only && !owner_state.online) continue;

        const auto* rec = find_owner_answer(owner_state, qname, qtype);
        if (!rec) continue;

        auto canon_data = canonicalize_rrset_data(rec->data);
        GroupKey key{rec->type, canon_data};

        std::uint64_t rtt = 0;
        if (const auto rtt_it = rtt_map.find(owner_id); rtt_it != rtt_map.end()) {
            rtt = rtt_it->second;
        }

        PeerCandidate cand;
        cand.node_id = owner_id;
        cand.claim_created_at = rec->created_at;
        cand.rtt_ms = rtt;
        cand.online = owner_state.online;

        auto& grp = group_map[key];
        if (grp.vote_count == 0) {
            grp.type = rec->type;
            grp.ttl = rec->ttl;
            grp.canonical_data = std::move(canon_data);
            grp.vote_count = 1;
            grp.oldest_claim_created = rec->created_at;
            grp.lowest_node_id = owner_id;
        } else {
            grp.vote_count += 1;
            grp.ttl = std::min(grp.ttl, rec->ttl);
            grp.oldest_claim_created = std::min(grp.oldest_claim_created, rec->created_at);
            if (owner_id < grp.lowest_node_id) {
                grp.lowest_node_id = owner_id;
            }
        }
        grp.candidates.push_back(cand);
    }

    if (group_map.empty()) {
        return result;
    }

    result.groups.reserve(group_map.size());
    for (auto& [_, grp] : group_map) {
        // Sort candidates within group: RTT ascending, then NodeId ascending
        std::sort(grp.candidates.begin(), grp.candidates.end(),
                  [](const PeerCandidate& a, const PeerCandidate& b) {
                      if (a.rtt_ms != b.rtt_ms) return a.rtt_ms < b.rtt_ms;
                      return a.node_id < b.node_id;
                  });
        result.groups.push_back(std::move(grp));
    }

    // Sort groups deterministically (protocol §8.4):
    // 1. vote_count descending
    // 2. oldest_claim_created ascending
    // 3. lowest_node_id ascending (lexicographical)
    std::sort(result.groups.begin(), result.groups.end(),
              [](const PeerGroup& a, const PeerGroup& b) {
                  if (a.vote_count != b.vote_count) return a.vote_count > b.vote_count;
                  if (a.oldest_claim_created != b.oldest_claim_created) {
                      return a.oldest_claim_created < b.oldest_claim_created;
                  }
                  return a.lowest_node_id < b.lowest_node_id;
              });

    // Conflict detection (SPEC §6.3.6)
    if (result.groups.size() > 1) {
        ConflictInfo ci;
        ci.name = qname;
        ci.type = qtype;
        ci.groups = result.groups;
        ci.local_divergence = false;
        if (local_canonical_data && !result.groups.empty()) {
            ci.local_divergence = (result.groups[0].canonical_data != *local_canonical_data);
        }
        result.conflict = std::move(ci);
    } else if (result.groups.size() == 1 && local_canonical_data) {
        if (result.groups[0].canonical_data != *local_canonical_data) {
            ConflictInfo ci;
            ci.name = qname;
            ci.type = qtype;
            ci.groups = result.groups;
            ci.local_divergence = true;
            result.conflict = std::move(ci);
        }
    }

    return result;
}

}  // namespace ldns::resolver
