#include "core/resolver/cache.hpp"

#include <algorithm>

namespace ldns::resolver {

std::vector<dns::ResourceRecord> CacheEntry::adjusted_answers(
    std::chrono::steady_clock::time_point now) const {
    std::vector<dns::ResourceRecord> out;
    out.reserve(answers.size());
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::seconds>(now - stored_at).count();
    for (const auto& item : answers) {
        dns::ResourceRecord rr = item.record;
        if (item.original_ttl > static_cast<std::uint64_t>(elapsed)) {
            rr.ttl = static_cast<std::uint32_t>(item.original_ttl - elapsed);
        } else {
            rr.ttl = 0;
        }
        out.push_back(std::move(rr));
    }
    return out;
}

std::vector<dns::ResourceRecord> CacheEntry::adjusted_authority(
    std::chrono::steady_clock::time_point now) const {
    std::vector<dns::ResourceRecord> out;
    out.reserve(authority.size());
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::seconds>(now - stored_at).count();
    for (const auto& item : authority) {
        dns::ResourceRecord rr = item.record;
        if (item.original_ttl > static_cast<std::uint64_t>(elapsed)) {
            rr.ttl = static_cast<std::uint32_t>(item.original_ttl - elapsed);
        } else {
            rr.ttl = 0;
        }
        out.push_back(std::move(rr));
    }
    return out;
}

DnsCache::DnsCache(std::size_t max_entries)
    : max_entries_(max_entries == 0 ? 1 : max_entries) {}

std::optional<CacheEntry> DnsCache::get(const dns::Name& name, std::uint16_t type,
                                        std::uint16_t qclass) {
    const CacheKey key{name, type, qclass};
    const auto now = std::chrono::steady_clock::now();

    std::unique_lock lock(mutex_);
    const auto it = map_.find(key);
    if (it == map_.end()) return std::nullopt;

    auto list_it = it->second;
    if (now >= list_it->entry.expires_at) {
        lru_list_.erase(list_it);
        map_.erase(it);
        return std::nullopt;
    }

    // Move to front (most recently used).
    if (list_it != lru_list_.begin()) {
        lru_list_.splice(lru_list_.begin(), lru_list_, list_it);
    }
    return list_it->entry;
}

void DnsCache::put_positive(const dns::Name& name, std::uint16_t type,
                            const std::vector<dns::ResourceRecord>& answers,
                            const std::vector<dns::ResourceRecord>& authority,
                            CacheSource source, std::uint32_t ttl_cap,
                            std::uint16_t qclass) {
    if (answers.empty()) return;
    const auto now = std::chrono::steady_clock::now();

    std::uint32_t min_ttl = answers.front().ttl;
    for (const auto& rr : answers) {
        min_ttl = std::min(min_ttl, rr.ttl);
    }
    min_ttl = std::min(min_ttl, ttl_cap);
    if (min_ttl == 0) return;  // Zero TTL should not be cached.

    CacheEntry entry;
    entry.rcode = dns::Rcode::NoError;
    entry.source = source;
    entry.stored_at = now;
    entry.expires_at = now + std::chrono::seconds(min_ttl);
    entry.is_negative = false;

    entry.answers.reserve(answers.size());
    for (const auto& rr : answers) {
        entry.answers.push_back(CachedRecord{rr, rr.ttl});
    }
    entry.authority.reserve(authority.size());
    for (const auto& rr : authority) {
        entry.authority.push_back(CachedRecord{rr, rr.ttl});
    }

    const CacheKey key{name, type, qclass};
    std::unique_lock lock(mutex_);
    const auto it = map_.find(key);
    if (it != map_.end()) {
        it->second->entry = std::move(entry);
        lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
        return;
    }

    if (map_.size() >= max_entries_) {
        evict_expired_or_lru_locked(now);
    }

    lru_list_.push_front(Item{key, std::move(entry)});
    map_[key] = lru_list_.begin();
}

void DnsCache::put_negative(const dns::Name& name, std::uint16_t type,
                            dns::Rcode rcode,
                            const std::optional<dns::ResourceRecord>& soa,
                            CacheSource source, std::uint32_t negative_ttl,
                            std::uint16_t qclass) {
    if (negative_ttl == 0) return;
    const auto now = std::chrono::steady_clock::now();

    CacheEntry entry;
    entry.rcode = rcode;
    entry.source = source;
    entry.stored_at = now;
    entry.expires_at = now + std::chrono::seconds(negative_ttl);
    entry.is_negative = true;

    if (soa) {
        entry.authority.push_back(CachedRecord{soa.value(), soa.value().ttl});
    }

    const CacheKey key{name, type, qclass};
    std::unique_lock lock(mutex_);
    const auto it = map_.find(key);
    if (it != map_.end()) {
        it->second->entry = std::move(entry);
        lru_list_.splice(lru_list_.begin(), lru_list_, it->second);
        return;
    }

    if (map_.size() >= max_entries_) {
        evict_expired_or_lru_locked(now);
    }

    lru_list_.push_front(Item{key, std::move(entry)});
    map_[key] = lru_list_.begin();
}

void DnsCache::evict_expired_or_lru_locked(std::chrono::steady_clock::time_point now) {
    // Try to find an expired item first.
    for (auto it = lru_list_.rbegin(); it != lru_list_.rend(); ++it) {
        if (now >= it->entry.expires_at) {
            auto base_it = std::next(it).base();
            map_.erase(base_it->key);
            lru_list_.erase(base_it);
            return;
        }
    }
    // Otherwise pop the least recently used (back).
    if (!lru_list_.empty()) {
        map_.erase(lru_list_.back().key);
        lru_list_.pop_back();
    }
}

void DnsCache::invalidate(const dns::Name& name) {
    std::unique_lock lock(mutex_);
    for (auto it = lru_list_.begin(); it != lru_list_.end();) {
        if (it->key.name == name) {
            map_.erase(it->key);
            it = lru_list_.erase(it);
        } else {
            ++it;
        }
    }
}

void DnsCache::clear() {
    std::unique_lock lock(mutex_);
    map_.clear();
    lru_list_.clear();
}

std::size_t DnsCache::size() const {
    std::shared_lock lock(mutex_);
    return map_.size();
}

}  // namespace ldns::resolver
