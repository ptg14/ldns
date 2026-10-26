#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/net/peer_manager.hpp"
#include "core/net/protocol_message.hpp"
#include "core/registry/registry.hpp"
#include "core/store/store.hpp"
#include "core/util/id.hpp"
#include "core/util/result.hpp"

namespace ldns {

struct SyncManagerOptions {
    NodeId self_id{};
    std::string self_name;
    std::uint64_t self_epoch{0};
    std::chrono::seconds offline_retention{std::chrono::hours(24 * 7)};
};

class SyncManager : public std::enable_shared_from_this<SyncManager> {
public:
    using CacheInvalidator = std::function<void(const std::string& name)>;

    static std::shared_ptr<SyncManager> create(
        SyncManagerOptions options,
        Store& store,
        Registry& registry,
        std::shared_ptr<net::PeerManager> peer_manager,
        CacheInvalidator cache_invalidator = nullptr);

    ~SyncManager() = default;

    void start();
    void stop();

    // Invoked by PeerManager on peer online/offline
    void on_peer_event(const net::PeerRecord& peer, bool online);

    // Invoked by PeerManager when sync-related protocol messages arrive
    void on_session_message(std::shared_ptr<net::Session> session, const net::ParsedMessage& msg);

    // Broadcasts local mutations (from DomainService add/set/rm/import or IP refresh)
    Result<void> broadcast_local_update(const std::vector<net::Op>& ops);

    // Sends SYNC_REQ to a specific session
    void send_sync_req(std::shared_ptr<net::Session> session,
                       std::optional<NodeId> specific_owner = std::nullopt);
    void request_sync_for_owner(const NodeId& owner_id);

    // Periodic maintenance
    void purge_offline(std::int64_t now_ts = 0);

private:
    SyncManager(SyncManagerOptions options,
                Store& store,
                Registry& registry,
                std::shared_ptr<net::PeerManager> peer_manager,
                CacheInvalidator cache_invalidator);

    void handle_sync_req(std::shared_ptr<net::Session> session, const net::SyncReqMessage& req);
    void handle_sync_data(std::shared_ptr<net::Session> session, const net::SyncDataMessage& data);
    void handle_sync_end(std::shared_ptr<net::Session> session, const net::SyncEndMessage& end);
    void handle_update(std::shared_ptr<net::Session> session, const net::UpdateMessage& update);

    void process_buffered_updates(const NodeId& owner_id, std::shared_ptr<net::Session> sender);

    struct BufferedUpdate {
        net::UpdateMessage update;
        std::chrono::steady_clock::time_point received_at;
    };

    SyncManagerOptions options_;
    Store& store_;
    Registry& registry_;
    std::shared_ptr<net::PeerManager> peer_manager_;
    CacheInvalidator cache_invalidator_;

    std::uint32_t next_sync_id_{1};
    mutable std::mutex mutex_;
    bool running_{false};

    // Incoming chunked snapshots: key: (session_raw_ptr, owner_id)
    std::map<std::pair<const void*, NodeId>, std::vector<net::Op>> incoming_snapshots_;

    // Buffered out-of-order updates per owner
    std::unordered_map<NodeId, std::vector<BufferedUpdate>> update_buffers_;
};

}  // namespace ldns
