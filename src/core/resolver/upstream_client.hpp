#pragma once

// Upstream DNS client (SPEC §6.2, §6.3, PLAN M3).
//
// Sends recursive DNS queries to configured upstream servers:
//   - Tries servers sequentially in the configured list.
//   - Falls back to configured fallback list if primary servers all fail.
//   - Sends UDP first; falls back to TCP when TC=1 is received.
//   - Timeout per query and retry count per server.
//   - Loop prevention: drops servers that point to own local listening endpoints.

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/dns/message.hpp"
#include "core/server/acl.hpp"
#include "core/util/result.hpp"

namespace ldns::resolver {

struct UpstreamConfig {
    std::vector<std::string> servers;
    std::vector<std::string> fallback;
    std::uint32_t timeout_ms{2000};
    std::uint32_t retries{1};
    // Local addresses to exclude from being contacted (loop guard).
    std::vector<std::string> excluded_endpoints;
};

struct UpstreamResponse {
    dns::Message message;
    std::string responder;
    std::uint64_t latency_us{0};
    bool used_tcp{false};
};

struct UpstreamTrace {
    std::string server;
    bool success{false};
    bool timeout{false};
    bool used_tcp{false};
    std::uint64_t latency_us{0};
    std::string error_message;
};

using UpstreamTraceCallback = std::function<void(const UpstreamTrace&)>;

class UpstreamClient {
public:
    explicit UpstreamClient(UpstreamConfig config);

    // Resolves a query through upstream servers.
    // Tries primary servers, then fallback servers if all primary fail.
    Result<UpstreamResponse> resolve(const dns::Message& query,
                                     UpstreamTraceCallback trace_cb = nullptr);

    void set_config(UpstreamConfig config);
    const UpstreamConfig& config() const noexcept { return config_; }

private:
    struct Endpoint {
        IpAddress ip;
        std::uint16_t port{53};
        std::string raw;
    };

    bool is_endpoint_excluded(const IpAddress& ip, std::uint16_t port) const;
    std::vector<Endpoint> parse_endpoints(const std::vector<std::string>& list) const;

    Result<UpstreamResponse> try_server(const Endpoint& target, const dns::Message& query,
                                        UpstreamTraceCallback trace_cb);

    Result<dns::Message> query_udp(const Endpoint& target,
                                   std::span<const std::uint8_t> wire,
                                   std::chrono::milliseconds timeout);

    Result<dns::Message> query_tcp(const Endpoint& target,
                                   std::span<const std::uint8_t> wire,
                                   std::chrono::milliseconds timeout);

    UpstreamConfig config_;
};

}  // namespace ldns::resolver
