#pragma once

// Resolution Pipeline (SPEC §6, PLAN M3).
//
// Resolution order (SPEC §6.2):
//   Step 0: Cache (hit -> return immediately)
//   Step 1: LocalZone (hit -> return, AA=1; name exists but different type -> NODATA, stop)
//   Step 2: PeerResolver (hook, empty in M3, filled in M6)
//   Step 3: Upstream (sequential list, timeout 2s, 1 retry, AA=0)
//
// Follows CNAME chains across all steps up to 8 hops.
// Records resolution traces for `--trace` and logs to QueryLog.

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/dns/local_zone.hpp"
#include "core/dns/message.hpp"
#include "core/dns/name.hpp"
#include "core/resolver/cache.hpp"
#include "core/resolver/query_log.hpp"
#include "core/resolver/upstream_client.hpp"
#include "core/server/query_handler.hpp"

namespace ldns::resolver {

struct TraceHop {
    ResolutionStep step{ResolutionStep::None};
    dns::Name qname;
    std::uint16_t qtype{0};
    std::string responder;
    std::uint64_t latency_us{0};
    dns::Rcode rcode{dns::Rcode::NoError};
    std::string note;
};

struct PipelineResult {
    dns::Message response;
    ResolutionStep resolved_step{ResolutionStep::None};
    std::vector<TraceHop> trace;
    std::uint64_t total_latency_us{0};
    bool from_cache{false};
};

struct PipelineConfig {
    std::uint32_t peer_ttl_cap{30};
    std::uint32_t upstream_ttl_cap{300};
    std::uint32_t negative_ttl{5};
    int max_cname_chain{8};
};

// Peer resolver hook for Step 2 (M6).
using PeerResolverHook = std::function<std::optional<dns::Message>(
    const dns::Name& name, std::uint16_t type, std::vector<TraceHop>& trace)>;

class ResolverPipeline {
public:
    ResolverPipeline(const dns::LocalZone& local_zone,
                     std::shared_ptr<DnsCache> cache,
                     std::shared_ptr<UpstreamClient> upstream,
                     std::shared_ptr<QueryLog> query_log,
                     PipelineConfig config = {});

    PipelineResult resolve(const dns::Message& query,
                           const QueryContext& context = {},
                           bool enable_trace = false);

    void set_peer_hook(PeerResolverHook hook);
    void set_config(PipelineConfig config);

    std::shared_ptr<DnsCache> cache() const noexcept { return cache_; }
    std::shared_ptr<UpstreamClient> upstream() const noexcept { return upstream_; }
    std::shared_ptr<QueryLog> query_log() const noexcept { return query_log_; }

private:
    const dns::LocalZone& local_zone_;
    std::shared_ptr<DnsCache> cache_;
    std::shared_ptr<UpstreamClient> upstream_;
    std::shared_ptr<QueryLog> query_log_;
    PipelineConfig config_;
    PeerResolverHook peer_hook_;
};

}  // namespace ldns::resolver
