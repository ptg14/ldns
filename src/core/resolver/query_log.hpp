#pragma once

// In-memory ring-buffer QueryLog (SPEC §5.4, §6, §10, PLAN M3).
//
// Records recent DNS queries and resolution steps (Cache, Local, Peer, Upstream),
// powering the `/logs` interactive command and latency tracking.

#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace ldns::resolver {

enum class ResolutionStep : char {
    Cache = 'C',
    Local = 'L',
    Peer = 'P',
    Upstream = 'U',
    None = '-',
};

std::string step_name(ResolutionStep step);

struct QueryLogEntry {
    std::uint64_t timestamp_ms{0};
    std::string client_ip;
    std::string qname;
    std::string qtype;
    ResolutionStep step{ResolutionStep::None};
    std::string rcode;
    std::uint64_t latency_us{0};
    std::string responder;  // Upstream or peer responder IP if applicable
};

using QueryLogCallback = std::function<void(const QueryLogEntry&)>;

class QueryLog {
public:
    explicit QueryLog(std::size_t max_entries = 10000);

    void record(QueryLogEntry entry);

    // Retrieves entries, up to `limit` latest entries.
    std::vector<QueryLogEntry> entries(std::size_t limit = 100) const;

    void set_listener(QueryLogCallback callback);
    void clear();
    std::size_t size() const;

private:
    std::size_t max_entries_;
    mutable std::mutex mutex_;
    std::deque<QueryLogEntry> ring_;
    QueryLogCallback callback_;
};

}  // namespace ldns::resolver
