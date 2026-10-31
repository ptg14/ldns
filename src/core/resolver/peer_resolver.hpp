#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/dns/local_zone.hpp"
#include "core/dns/message.hpp"
#include "core/dns/name.hpp"
#include "core/net/peer_manager.hpp"
#include "core/registry/registry.hpp"
#include "core/resolver/peer_ranking.hpp"
#include "core/resolver/pipeline.hpp"

namespace ldns::resolver {

struct PeerResolverConfig {
    std::chrono::milliseconds hedge_ms{250};
    std::chrono::milliseconds budget_ms{1500};
    std::uint32_t peer_ttl_cap{30};
    std::uint32_t serve_stale_ttl{10};
    bool serve_stale{false};
};

struct ConflictRecord {
    dns::Name name;
    std::uint16_t type{1};
    std::vector<PeerGroup> groups;
    bool local_divergence{false};
    std::chrono::system_clock::time_point detected_at{};
};

class PeerResolver : public std::enable_shared_from_this<PeerResolver> {
public:
    using SyncTriggerCallback = std::function<void(const NodeId& owner)>;
    using ConflictCallback = std::function<void(const ConflictRecord&)>;
    using SeqProvider = std::function<std::uint64_t()>;

    PeerResolver(std::shared_ptr<Registry> registry,
                 std::shared_ptr<net::PeerManager> peer_manager,
                 const dns::LocalZone& local_zone,
                 PeerResolverConfig config = {},
                 std::uint64_t self_epoch = 0,
                 SeqProvider seq_provider = nullptr);

    ~PeerResolver();

    void set_sync_trigger(SyncTriggerCallback cb);
    void set_conflict_callback(ConflictCallback cb);
    void set_config(PeerResolverConfig config);
    PeerResolverConfig config() const;
    void set_self_epoch(std::uint64_t epoch);
    void set_peer_manager(std::shared_ptr<net::PeerManager> pm);

    // Resolution hook for ResolverPipeline (Step 2)
    std::optional<dns::Message> resolve(
        const dns::Name& name, std::uint16_t type, std::vector<TraceHop>& trace);

    // Inbound request handler (evaluates strictly LocalZone per SPEC §6.3.3, §8.2)
    void handle_resolve_request(std::shared_ptr<net::Session> session,
                                const net::ResolveMessage& req);

    // Inbound response handler
    void handle_resolve_response(std::shared_ptr<net::Session> session,
                                 const net::ResolveResponseMessage& resp);

    // Conflicts management (/conflicts and /info)
    std::vector<ConflictRecord> conflicts() const;
    std::optional<ConflictRecord> get_conflict(const dns::Name& name, std::uint16_t type) const;
    void clear_conflicts();

private:
    struct PendingQuery {
        std::mutex mtx;
        std::condition_variable cv;
        std::optional<net::ResolveResponseMessage> response;
        bool done{false};
    };

    std::shared_ptr<Registry> registry_;
    std::shared_ptr<net::PeerManager> peer_manager_;
    const dns::LocalZone& local_zone_;
    PeerResolverConfig config_;
    std::uint64_t self_epoch_{0};
    SeqProvider seq_provider_;

    SyncTriggerCallback sync_trigger_;
    ConflictCallback conflict_cb_;

    mutable std::mutex mutex_;
    std::uint32_t next_req_id_{1};
    std::unordered_map<std::uint32_t, std::shared_ptr<PendingQuery>> pending_queries_;

    // Map key: (name_string, type)
    std::map<std::pair<std::string, std::uint16_t>, ConflictRecord> conflicts_;
};

}  // namespace ldns::resolver
