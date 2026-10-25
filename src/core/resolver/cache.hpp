#pragma once

// In-memory LRU DNS cache (SPEC §6.2, §6.3, PLAN M3).
//
// Features:
//   - Thread-safe: read/write synchronization with std::shared_mutex.
//   - LRU eviction when capacity (default 50,000 entries) is reached.
//   - Decrements TTL of returned ResourceRecords according to elapsed time.
//   - Positive caching capped by peer_ttl_cap (30s) or upstream_ttl_cap (300s).
//   - Negative caching (NXDOMAIN / NODATA) with negative_ttl (default 5s).
//   - Invalidation support for real-time peer/zone updates.

#include <chrono>
#include <cstdint>
#include <list>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/dns/message.hpp"
#include "core/dns/name.hpp"

namespace ldns::resolver {

enum class CacheSource {
    Local,
    Peer,
    Upstream,
};

struct CachedRecord {
    dns::ResourceRecord record;
    // Original TTL when stored, used to decrement TTL on retrieval.
    std::uint32_t original_ttl{0};
};

struct CacheEntry {
    dns::Rcode rcode{dns::Rcode::NoError};
    std::vector<CachedRecord> answers;
    std::vector<CachedRecord> authority;
    CacheSource source{CacheSource::Upstream};
    std::chrono::steady_clock::time_point stored_at;
    std::chrono::steady_clock::time_point expires_at;
    bool is_negative{false};

    // Synthesizes a response Message with decremented TTLs.
    std::vector<dns::ResourceRecord> adjusted_answers(
        std::chrono::steady_clock::time_point now) const;
    std::vector<dns::ResourceRecord> adjusted_authority(
        std::chrono::steady_clock::time_point now) const;
};

struct CacheKey {
    dns::Name name;
    std::uint16_t type{0};
    std::uint16_t qclass{dns::kClassIn};

    friend bool operator==(const CacheKey& a, const CacheKey& b) noexcept {
        return a.name == b.name && a.type == b.type && a.qclass == b.qclass;
    }
};

struct CacheKeyHash {
    std::size_t operator()(const CacheKey& key) const noexcept {
        std::size_t h = std::hash<dns::Name>{}(key.name);
        h ^= (static_cast<std::size_t>(key.type) << 16) * 2654435761ull;
        h ^= static_cast<std::size_t>(key.qclass) * 11400714819323198485ull;
        return h;
    }
};

class DnsCache {
public:
    explicit DnsCache(std::size_t max_entries = 50000);

    // Look up an entry. Returns nullopt if absent or expired.
    // If found, updates LRU order.
    std::optional<CacheEntry> get(const dns::Name& name, std::uint16_t type,
                                 std::uint16_t qclass = dns::kClassIn);

    // Stores positive response answers.
    void put_positive(const dns::Name& name, std::uint16_t type,
                      const std::vector<dns::ResourceRecord>& answers,
                      const std::vector<dns::ResourceRecord>& authority,
                      CacheSource source, std::uint32_t ttl_cap,
                      std::uint16_t qclass = dns::kClassIn);

    // Stores negative response (NXDOMAIN or NODATA).
    void put_negative(const dns::Name& name, std::uint16_t type,
                      dns::Rcode rcode,
                      const std::optional<dns::ResourceRecord>& soa,
                      CacheSource source, std::uint32_t negative_ttl,
                      std::uint16_t qclass = dns::kClassIn);

    // Invalidates all cached records matching the name (exact or any type).
    void invalidate(const dns::Name& name);

    // Clears the entire cache.
    void clear();

    std::size_t size() const;
    std::size_t max_entries() const noexcept { return max_entries_; }

private:
    struct Item {
        CacheKey key;
        CacheEntry entry;
    };

    using List = std::list<Item>;
    using Map = std::unordered_map<CacheKey, List::iterator, CacheKeyHash>;

    void evict_expired_or_lru_locked(std::chrono::steady_clock::time_point now);

    std::size_t max_entries_;
    mutable std::shared_mutex mutex_;
    List lru_list_;
    Map map_;
};

}  // namespace ldns::resolver
