#include "core/resolver/query_log.hpp"

namespace ldns::resolver {

std::string step_name(ResolutionStep step) {
    switch (step) {
        case ResolutionStep::Cache: return "Cache";
        case ResolutionStep::Local: return "Local";
        case ResolutionStep::Peer: return "Peer";
        case ResolutionStep::Upstream: return "Upstream";
        default: return "None";
    }
}

QueryLog::QueryLog(std::size_t max_entries)
    : max_entries_(max_entries == 0 ? 1 : max_entries) {}

void QueryLog::record(QueryLogEntry entry) {
    QueryLogCallback cb;
    {
        std::lock_guard lock(mutex_);
        if (ring_.size() >= max_entries_) {
            ring_.pop_front();
        }
        ring_.push_back(entry);
        cb = callback_;
    }
    if (cb) cb(entry);
}

std::vector<QueryLogEntry> QueryLog::entries(std::size_t limit) const {
    std::lock_guard lock(mutex_);
    if (limit == 0 || ring_.empty()) return {};

    const std::size_t count = std::min(limit, ring_.size());
    std::vector<QueryLogEntry> out;
    out.reserve(count);

    // Return latest entries first
    auto it = ring_.rbegin();
    for (std::size_t i = 0; i < count; ++i, ++it) {
        out.push_back(*it);
    }
    return out;
}

void QueryLog::set_listener(QueryLogCallback callback) {
    std::lock_guard lock(mutex_);
    callback_ = std::move(callback);
}

void QueryLog::clear() {
    std::lock_guard lock(mutex_);
    ring_.clear();
}

std::size_t QueryLog::size() const {
    std::lock_guard lock(mutex_);
    return ring_.size();
}

}  // namespace ldns::resolver
