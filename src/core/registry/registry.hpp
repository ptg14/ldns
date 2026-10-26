#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/dns/name.hpp"
#include "core/net/protocol_message.hpp"
#include "core/store/store.hpp"
#include "core/util/id.hpp"
#include "core/util/result.hpp"

namespace ldns {

using net::Op;

struct OwnerRecord {
    dns::Name name;
    std::string name_str;
    std::uint16_t type{1};
    std::uint32_t ttl{0};
    std::vector<std::vector<std::uint8_t>> data;
    std::uint64_t created_at{0};
    std::uint64_t ts{0};
};

struct OwnerState {
    NodeId owner_id{};
    std::string name;
    std::uint64_t epoch{0};
    std::uint64_t last_seq{0};
    bool online{false};
    std::int64_t last_seen{0};
    // Key: (name_str, type)
    std::map<std::pair<std::string, std::uint16_t>, OwnerRecord> records;
};

struct RecordClaim {
    NodeId owner_id{};
    OwnerRecord record;
    bool owner_online{false};
};

enum class RecordEventKind {
    Put,
    Del
};

struct RecordEvent {
    RecordEventKind kind{RecordEventKind::Put};
    NodeId owner_id{};
    OwnerRecord record;
};

// Immutable snapshot for lock-free hot path reads
class RegistrySnapshot {
public:
    RegistrySnapshot() = default;

    const std::unordered_map<NodeId, OwnerState>& owners() const noexcept {
        return owners_;
    }

    const OwnerState* find_owner(const NodeId& id) const noexcept {
        auto it = owners_.find(id);
        if (it != owners_.end()) return &it->second;
        return nullptr;
    }

    // Exact or wildcard claim lookup
    std::vector<RecordClaim> find_claims(const dns::Name& name, std::uint16_t type, bool online_only = false) const;

    // Exact name match only
    std::vector<RecordClaim> find_claims_exact(const dns::Name& name, std::uint16_t type, bool online_only = false) const;

    std::vector<OwnerRecord> records_for_owner(const NodeId& id) const;

    std::size_t total_record_count() const noexcept {
        return total_records_;
    }

private:
    friend class Registry;
    std::unordered_map<NodeId, OwnerState> owners_;
    std::unordered_map<dns::Name, std::vector<RecordClaim>> by_name_;
    std::size_t total_records_{0};
};

class Registry {
public:
    using ChangeListener = std::function<void(const RecordEvent&)>;

    Registry();
    ~Registry() = default;

    Registry(const Registry&) = delete;
    Registry& operator=(const Registry&) = delete;

    std::shared_ptr<const RegistrySnapshot> snapshot() const;

    void add_listener(ChangeListener listener);

    Result<void> apply_ops(const NodeId& owner_id, std::uint64_t epoch, std::uint64_t seq,
                           const std::vector<Op>& ops);

    Result<void> reset_owner_snapshot(const NodeId& owner_id, std::uint64_t epoch,
                                      std::uint64_t to_seq, const std::vector<Op>& ops);

    void set_owner_online(const NodeId& owner_id, bool online, const std::string& name = "",
                          std::int64_t last_seen = 0, std::uint64_t epoch = 0,
                          std::uint64_t seq = 0);

    void remove_owner(const NodeId& owner_id);

    std::vector<NodeId> purge_offline_owners(std::chrono::seconds retention, std::int64_t now_ts);

    void init_from_store(const std::vector<StoredPeer>& peers,
                         const std::vector<PeerRecordRow>& peer_records,
                         const NodeId& self_id,
                         std::uint64_t self_epoch,
                         std::uint64_t self_seq,
                         const std::vector<RecordRow>& my_records);

private:
    void rebuild_snapshot_locked();
    void notify_listeners(const std::vector<RecordEvent>& events);

    mutable std::mutex mutex_;
    std::unordered_map<NodeId, OwnerState> owners_;
    std::shared_ptr<const RegistrySnapshot> snapshot_;
    std::vector<ChangeListener> listeners_;
};

}  // namespace ldns
