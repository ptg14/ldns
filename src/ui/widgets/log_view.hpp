#pragma once

#include <string>
#include <vector>

namespace ldns::ui {

struct QueryLogEntry {
    std::string timestamp; // "14:02:11.104"
    std::string client_ip; // "127.0.0.1"
    std::string qtype;     // "A"
    std::string domain;    // "app.lan"
    std::string step;      // "[L:Local]", "[P:Peer]", "[U:Upstream]"
    std::string rcode;     // "NOERROR", "NXDOMAIN", "SERVFAIL"
    std::string latency;   // "180 µs", "2.4 ms"
};

class LogView {
public:
    static std::vector<std::string> render(const std::vector<QueryLogEntry>& entries);
    static std::string format_entry(const QueryLogEntry& entry);
};

}  // namespace ldns::ui
