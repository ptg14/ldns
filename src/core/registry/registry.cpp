#include "core/registry/registry.hpp"

#include <algorithm>

namespace ldns {

std::vector<RecordClaim> RegistrySnapshot::find_claims_exact(const dns::Name& name,
                                                             std::uint16_t type,
                                                             bool online_only) const {
    const auto it = by_name_.find(name);
    if (it == by_name_.end()) return {};

    std::vector<RecordClaim> out;
    for (const auto& claim : it->second) {
        if (online_only && !claim.owner_online) continue;
        if (type == 255 || claim.record.type == type) {
            out.push_back(claim);
        }
    }
    return out;
}

std::vector<RecordClaim> RegistrySnapshot::find_claims(const dns::Name& name,
                                                       std::uint16_t type,
                                                       bool online_only) const {
    auto exact = find_claims_exact(name, type, online_only);
    if (!exact.empty()) return exact;

    // Wildcard: climb ancestors, most specific first ("*.a.b" before "*.b")
    for (dns::Name ancestor = name.parent(); !ancestor.is_root(); ancestor = ancestor.parent()) {
        auto wildcard = dns::Name::make_wildcard(ancestor);
        if (!wildcard) break;
        auto wildcard_claims = find_claims_exact(wildcard.value(), type, online_only);
        if (!wildcard_claims.empty()) return wildcard_claims;
    }

    return {};
}

std::vector<OwnerRecord> RegistrySnapshot::records_for_owner(const NodeId& id) const {
    const auto it = owners_.find(id);
    if (it == owners_.end()) return {};

    std::vector<OwnerRecord> out;
    out.reserve(it->second.records.size());
    for (const auto& [_, rec] : it->second.records) {
        out.push_back(rec);
    }
    return out;
}

Registry::Registry() {
    rebuild_snapshot_locked();
}

std::shared_ptr<const RegistrySnapshot> Registry::snapshot() const {
    std::lock_guard lock(mutex_);
    return snapshot_;
}

void Registry::add_listener(ChangeListener listener) {
    std::lock_guard lock(mutex_);
    listeners_.push_back(std::move(listener));
}

Result<void> Registry::apply_ops(const NodeId& owner_id, std::uint64_t epoch,
                                 std::uint64_t seq, const std::vector<Op>& ops) {
    std::vector<RecordEvent> events;
    {
        std::lock_guard lock(mutex_);
        auto& owner = owners_[owner_id];
        owner.owner_id = owner_id;

        if (epoch > owner.epoch) {
            owner.records.clear();
            owner.epoch = epoch;
            owner.last_seq = 0;
        } else if (epoch < owner.epoch) {
            return Error{840, "stale epoch in update"};
        }

        if (seq <= owner.last_seq && owner.last_seq > 0) {
            return {};  // Duplicate or old sequence
        }

        for (const auto& op : ops) {
            auto parsed_name = dns::Name::from_presentation(op.name);
            if (!parsed_name) continue;

            if (op.op == "put") {
                OwnerRecord rec;
                rec.name = parsed_name.value();
                rec.name_str = op.name;
                rec.type = op.type;
                rec.ttl = op.ttl;
                rec.data = op.data;
                rec.created_at = op.created;
                rec.ts = op.ts;

                owner.records[{op.name, op.type}] = rec;

                RecordEvent ev;
                ev.kind = RecordEventKind::Put;
                ev.owner_id = owner_id;
                ev.record = std::move(rec);
                events.push_back(std::move(ev));
            } else if (op.op == "del") {
                auto it = owner.records.find({op.name, op.type});
                if (it != owner.records.end()) {
                    RecordEvent ev;
                    ev.kind = RecordEventKind::Del;
                    ev.owner_id = owner_id;
                    ev.record = it->second;
                    events.push_back(std::move(ev));
                    owner.records.erase(it);
                }
            }
        }

        owner.last_seq = seq;
        rebuild_snapshot_locked();
    }

    notify_listeners(events);
    return {};
}

Result<void> Registry::reset_owner_snapshot(const NodeId& owner_id, std::uint64_t epoch,
                                            std::uint64_t to_seq, const std::vector<Op>& ops) {
    std::vector<RecordEvent> events;
    {
        std::lock_guard lock(mutex_);
        auto& owner = owners_[owner_id];
        owner.owner_id = owner_id;
        owner.epoch = epoch;
        owner.last_seq = to_seq;
        owner.records.clear();

        for (const auto& op : ops) {
            auto parsed_name = dns::Name::from_presentation(op.name);
            if (!parsed_name) continue;

            OwnerRecord rec;
            rec.name = parsed_name.value();
            rec.name_str = op.name;
            rec.type = op.type;
            rec.ttl = op.ttl;
            rec.data = op.data;
            rec.created_at = op.created;
            rec.ts = op.ts;

            owner.records[{op.name, op.type}] = rec;

            RecordEvent ev;
            ev.kind = RecordEventKind::Put;
            ev.owner_id = owner_id;
            ev.record = std::move(rec);
            events.push_back(std::move(ev));
        }

        rebuild_snapshot_locked();
    }

    notify_listeners(events);
    return {};
}

void Registry::set_owner_online(const NodeId& owner_id, bool online, const std::string& name,
                                std::int64_t last_seen, std::uint64_t epoch,
                                std::uint64_t seq) {
    std::lock_guard lock(mutex_);
    auto& owner = owners_[owner_id];
    owner.owner_id = owner_id;
    owner.online = online;
    if (!name.empty()) {
        owner.name = name;
    }
    if (last_seen > 0) {
        owner.last_seen = last_seen;
    }
    if (epoch > 0 && owner.epoch == 0) {
        owner.epoch = epoch;
        owner.last_seq = seq;
    }
    rebuild_snapshot_locked();
}

void Registry::remove_owner(const NodeId& owner_id) {
    std::lock_guard lock(mutex_);
    owners_.erase(owner_id);
    rebuild_snapshot_locked();
}

std::vector<NodeId> Registry::purge_offline_owners(std::chrono::seconds retention,
                                                   std::int64_t now_ts) {
    std::vector<NodeId> purged;
    {
        std::lock_guard lock(mutex_);
        const auto retention_sec = retention.count();
        for (auto it = owners_.begin(); it != owners_.end();) {
            if (!it->second.online && it->second.last_seen > 0 &&
                (now_ts - it->second.last_seen >= retention_sec)) {
                purged.push_back(it->first);
                it = owners_.erase(it);
            } else {
                ++it;
            }
        }
        if (!purged.empty()) {
            rebuild_snapshot_locked();
        }
    }
    return purged;
}

void Registry::init_from_store(const std::vector<StoredPeer>& peers,
                               const std::vector<PeerRecordRow>& peer_records,
                               const NodeId& self_id,
                               std::uint64_t self_epoch,
                               std::uint64_t self_seq,
                               const std::vector<RecordRow>& my_records) {
    std::lock_guard lock(mutex_);
    owners_.clear();

    for (const auto& p : peers) {
        auto& s = owners_[p.node_id];
        s.owner_id = p.node_id;
        s.name = p.name;
        s.epoch = p.epoch;
        s.last_seq = p.seq;
        s.online = false;
        s.last_seen = p.last_seen;
    }

    for (const auto& pr : peer_records) {
        auto parsed = dns::Name::from_presentation(pr.name);
        if (!parsed) continue;

        auto& s = owners_[pr.owner_id];
        s.owner_id = pr.owner_id;

        OwnerRecord rec;
        rec.name = parsed.value();
        rec.name_str = pr.name;
        rec.type = pr.type;
        rec.ttl = pr.ttl;
        rec.data = pr.data;
        rec.created_at = pr.created_at;
        rec.ts = pr.created_at;

        s.records[{pr.name, pr.type}] = std::move(rec);
    }

    auto& self = owners_[self_id];
    self.owner_id = self_id;
    self.epoch = self_epoch;
    self.last_seq = self_seq;
    self.online = true;

    for (const auto& mr : my_records) {
        auto parsed = dns::Name::from_presentation(mr.name);
        if (!parsed) continue;

        OwnerRecord rec;
        rec.name = parsed.value();
        rec.name_str = mr.name;
        rec.type = mr.type;
        rec.ttl = mr.ttl;
        rec.data = mr.data;
        rec.created_at = mr.created;
        rec.ts = mr.updated;

        self.records[{mr.name, mr.type}] = std::move(rec);
    }

    rebuild_snapshot_locked();
}

void Registry::rebuild_snapshot_locked() {
    auto snap = std::make_shared<RegistrySnapshot>();
    snap->owners_ = owners_;

    std::size_t total = 0;
    for (const auto& [owner_id, owner_state] : owners_) {
        for (const auto& [_, rec] : owner_state.records) {
            RecordClaim claim;
            claim.owner_id = owner_id;
            claim.record = rec;
            claim.owner_online = owner_state.online;
            snap->by_name_[rec.name].push_back(std::move(claim));
            total++;
        }
    }
    snap->total_records_ = total;
    snapshot_ = std::move(snap);
}

void Registry::notify_listeners(const std::vector<RecordEvent>& events) {
    if (events.empty()) return;
    std::vector<ChangeListener> listeners_copy;
    {
        std::lock_guard lock(mutex_);
        listeners_copy = listeners_;
    }
    for (const auto& ev : events) {
        for (const auto& l : listeners_copy) {
            l(ev);
        }
    }
}

}  // namespace ldns
