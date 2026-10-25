#pragma once

// Pure request/response logic for the DNS server (SPEC §8), independent of
// sockets so it can be unit tested. The transport layer (dns_server.cpp) adds
// ACL, rate limiting, framing and truncation.
//
// M1 serves answers from LocalZone only. The Resolver pipeline (M3) will
// replace the zone lookup without changing the wire-facing behaviour here.

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "core/dns/local_zone.hpp"
#include "core/dns/message.hpp"

namespace ldns {

struct HandlerConfig {
    // Advertised EDNS0 payload size; also the UDP response ceiling.
    std::uint16_t edns_udp_payload{1232};
    bool recursive_available{true};
};

struct QueryContext {
    bool tcp{false};
    // Client's EDNS0 payload size; 512 when the query has no OPT record.
    std::uint16_t udp_payload{512};
    std::string client_ip;

    QueryContext() = default;
    QueryContext(bool t, std::uint16_t u = 512, std::string ip = {})
        : tcp(t), udp_payload(u), client_ip(std::move(ip)) {}
};

namespace resolver {
class ResolverPipeline;
}

struct HandlerResponse {
    dns::Message message;
    bool from_local_zone{false};
};

class QueryHandler {
public:
    explicit QueryHandler(const dns::LocalZone& zone, HandlerConfig config = {},
                          std::shared_ptr<resolver::ResolverPipeline> pipeline = nullptr);

    // Returns nullopt when the packet must be dropped silently (not a query,
    // unparseable, unsupported transport-level rubbish).
    std::optional<HandlerResponse> handle(std::span<const std::uint8_t> packet,
                                          const QueryContext& context) const;

    const HandlerConfig& config() const noexcept { return config_; }

private:
    const dns::LocalZone& zone_;
    HandlerConfig config_;
    std::shared_ptr<resolver::ResolverPipeline> pipeline_;
};

}  // namespace ldns
