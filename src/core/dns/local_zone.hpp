#pragma once

// In-memory zone owned by this node (SPEC §5, §9).
//
// Lookup order for a query (SPEC §6.2 step 1, §8.2):
//   1. exact name match;
//   2. wildcard match, most specific ancestor first ("*.a.b" before "*.b");
//   3. otherwise NXDOMAIN or NODATA (name exists, type does not).
// CNAME is returned as a single step; the resolver follows the chain through
// the whole pipeline (SPEC §6.3.4).
//
// Thread safety: reads take a shared lock, writes an exclusive lock. The DNS
// hot path will move to immutable snapshots when sync lands (PLAN M5).

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/dns/message.hpp"
#include "core/dns/name.hpp"
#include "core/util/result.hpp"

namespace ldns::dns {

inline constexpr std::size_t kMaxRrsetValues = 64;
inline constexpr std::uint32_t kMaxTtl = 2'147'483'647;

enum class LookupKind {
    Answer,   // RRset(s) found for the requested type
    Cname,    // name exists as CNAME only; follow cname_target
    NoData,   // name exists, requested type does not
    NxDomain  // name does not exist (no exact, no wildcard)
};

struct LookupOutcome {
    LookupKind kind{LookupKind::NxDomain};
    std::vector<ResourceRecord> answers;
    std::optional<Name> cname_target;
};

class LocalZone {
public:
    // Inserts or replaces the RRset for (name, type). `data` must already be in
    // wire format for the type (see message.hpp encode_rdata_*).
    // Rejects: empty/oversized rrsets, invalid TTL, wildcard middle labels,
    // CNAME mixed with other types, CNAME with != 1 value.
    Result<void> upsert(const Name& name, std::uint16_t type, std::uint32_t ttl,
                        std::vector<std::vector<std::uint8_t>> data);

    // Removes one RRset. Returns true if it existed.
    bool remove(const Name& name, std::uint16_t type);

    bool contains(const Name& name, std::uint16_t type) const;

    // Looks up (name, qtype). `ANY` returns every RRset at the name.
    LookupOutcome lookup(const Name& name, std::uint16_t qtype) const;

    // SOA to attach to negative answers: nearest ancestor (including `name`)
    // that has an SOA record, else nullopt.
    std::optional<ResourceRecord> negative_soa(const Name& name) const;

    // All RRsets, sorted by (name, type) — for export, snapshots and tests.
    struct Entry {
        Name name;
        std::uint16_t type{};
        std::uint32_t ttl{};
        std::vector<std::vector<std::uint8_t>> data;
    };
    std::vector<Entry> entries() const;

    std::size_t name_count() const;
    std::size_t record_count() const;
    bool empty() const;
    void clear();

    // Validation shared with the domain service and the peer op validator
    // (protocol.md §2.3, §10). `type` must be one of the v1 RR types.
    static Result<void> validate_rrset(std::uint16_t type, std::uint32_t ttl,
                                       const std::vector<std::vector<std::uint8_t>>& data);

private:
    struct Rrset {
        std::uint32_t ttl{};
        std::vector<std::vector<std::uint8_t>> data;
    };
    using TypeMap = std::map<std::uint16_t, Rrset>;
    using NameMap = std::unordered_map<Name, TypeMap>;

    static std::vector<ResourceRecord> make_answers(const Name& owner, const TypeMap& types,
                                                    std::uint16_t type);
    static std::optional<Name> cname_of(const TypeMap& types);
    std::size_t record_count_unlocked() const;

    mutable std::shared_mutex mutex_;
    NameMap names_;
};

}  // namespace ldns::dns
