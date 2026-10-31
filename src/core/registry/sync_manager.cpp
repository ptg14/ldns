#include "core/registry/sync_manager.hpp"

#include <algorithm>
#include <ctime>

namespace ldns {

std::shared_ptr<SyncManager> SyncManager::create(
    SyncManagerOptions options,
    Store& store,
    Registry& registry,
    std::shared_ptr<net::PeerManager> peer_manager,
    CacheInvalidator cache_invalidator) {
    return std::shared_ptr<SyncManager>(new SyncManager(
        std::move(options), store, registry, std::move(peer_manager), std::move(cache_invalidator)));
}

SyncManager::SyncManager(SyncManagerOptions options,
                         Store& store,
                         Registry& registry,
                         std::shared_ptr<net::PeerManager> peer_manager,
                         CacheInvalidator cache_invalidator)
    : options_(std::move(options)),
      store_(store),
      registry_(registry),
      peer_manager_(std::move(peer_manager)),
      cache_invalidator_(std::move(cache_invalidator)) {}

void SyncManager::start() {
    std::lock_guard lock(mutex_);
    if (running_) return;
    running_ = true;

    if (peer_manager_) {
        auto self = shared_from_this();
        peer_manager_->set_sync_handler(
            [self](std::shared_ptr<net::Session> session, const net::ParsedMessage& msg) {
                self->on_session_message(std::move(session), msg);
            });
    }
}

void SyncManager::stop() {
    std::lock_guard lock(mutex_);
    running_ = false;
    if (peer_manager_) {
        peer_manager_->set_sync_handler(nullptr);
    }
    incoming_snapshots_.clear();
    update_buffers_.clear();
}

void SyncManager::on_peer_event(const net::PeerRecord& peer, bool online) {
    const auto now = std::time(nullptr);
    registry_.set_owner_online(peer.node_id, online, peer.name, now, peer.epoch, peer.seq);

    if (online && peer.session && peer.session->state() == net::SessionState::Established) {
        send_sync_req(peer.session);
    }
}

void SyncManager::on_session_message(std::shared_ptr<net::Session> session,
                                     const net::ParsedMessage& msg) {
    if (session && session->state() != net::SessionState::Established) return;

    switch (msg.type) {
        case net::MessageType::SyncReq:
            if (msg.sync_req) handle_sync_req(session, *msg.sync_req);
            break;
        case net::MessageType::SyncData:
            if (msg.sync_data) handle_sync_data(session, *msg.sync_data);
            break;
        case net::MessageType::SyncEnd:
            if (msg.sync_end) handle_sync_end(session, *msg.sync_end);
            break;
        case net::MessageType::Update:
            if (msg.update) handle_update(session, *msg.update);
            break;
        default:
            break;
    }
}

void SyncManager::send_sync_req(std::shared_ptr<net::Session> session,
                                std::optional<NodeId> specific_owner) {
    if (!session || session->state() != net::SessionState::Established) return;

    net::SyncReqMessage req;
    {
        std::lock_guard lock(mutex_);
        req.id = next_sync_id_++;
    }

    auto snap = registry_.snapshot();
    if (specific_owner) {
        const auto* owner = snap->find_owner(*specific_owner);
        if (owner) {
            req.owners.push_back({*specific_owner, owner->epoch, owner->last_seq});
        } else {
            req.owners.push_back({*specific_owner, 0, 0});
        }
    } else {
        for (const auto& [id, owner] : snap->owners()) {
            req.owners.push_back({id, owner.epoch, owner.last_seq});
        }
    }

    auto frame = net::encode_sync_req_frame(req);
    if (frame) session->send_frame(std::move(frame.value()));
}

void SyncManager::handle_sync_req(std::shared_ptr<net::Session> session,
                                  const net::SyncReqMessage& req) {
    if (!session) return;

    auto snap = registry_.snapshot();

    // Map requested owners for O(1) lookup
    std::unordered_map<NodeId, net::SyncReqOwner> requested;
    for (const auto& ro : req.owners) {
        requested[ro.owner] = ro;
    }

    for (const auto& [owner_id, owner_state] : snap->owners()) {
        const auto req_it = requested.find(owner_id);
        const bool not_in_req = (req_it == requested.end());
        const bool epoch_mismatch = (!not_in_req && req_it->second.epoch != owner_state.epoch);
        const uint64_t their_since = (!not_in_req) ? req_it->second.since_seq : 0;

        if (not_in_req || epoch_mismatch || their_since < owner_state.last_seq) {
            bool sent_delta = false;

            // Attempt delta if it's our own zone and op_log is available
            if (!not_in_req && !epoch_mismatch && owner_id == options_.self_id) {
                auto oldest_res = store_.oldest_op_seq();
                if (oldest_res && oldest_res.value() <= their_since + 1) {
                    auto ops_res = store_.ops_since(their_since, owner_state.last_seq);
                    if (ops_res) {
                        // Compress ops: keep only the latest op per (name, type)
                        std::map<std::pair<std::string, std::uint16_t>, net::Op> compressed;
                        for (const auto& row : ops_res.value()) {
                            net::Op op;
                            op.op = row.op;
                            op.name = row.name;
                            op.type = row.type;
                            op.ttl = row.ttl;
                            op.data = row.data;
                            op.created = row.created_at;
                            op.ts = row.ts;
                            compressed[{row.name, row.type}] = std::move(op);
                        }

                        if (compressed.size() <= 1000) {
                            net::SyncDataMessage delta;
                            delta.id = req.id;
                            delta.owner = owner_id;
                            delta.epoch = owner_state.epoch;
                            delta.from_seq = their_since;
                            delta.to_seq = owner_state.last_seq;
                            delta.snapshot = false;
                            delta.chunk = 0;
                            delta.last = true;
                            for (auto& [_, op] : compressed) {
                                delta.ops.push_back(std::move(op));
                            }
                            auto frame = net::encode_sync_data_frame(delta);
                            if (frame) session->send_frame(std::move(frame.value()));
                            sent_delta = true;
                        }
                    }
                }
            }

            if (!sent_delta) {
                // Send Snapshot in chunks of <= 1000 ops
                std::vector<net::Op> all_ops;
                all_ops.reserve(owner_state.records.size());
                for (const auto& [_, rec] : owner_state.records) {
                    net::Op op;
                    op.op = "put";
                    op.name = rec.name_str;
                    op.type = rec.type;
                    op.ttl = rec.ttl;
                    op.data = rec.data;
                    op.created = rec.created_at;
                    op.ts = rec.ts;
                    all_ops.push_back(std::move(op));
                }

                constexpr std::size_t kChunkSize = 1000;
                const std::size_t total_ops = all_ops.size();
                const std::size_t total_chunks = (total_ops + kChunkSize - 1) / kChunkSize;

                if (total_chunks == 0) {
                    net::SyncDataMessage chunk;
                    chunk.id = req.id;
                    chunk.owner = owner_id;
                    chunk.epoch = owner_state.epoch;
                    chunk.from_seq = 0;
                    chunk.to_seq = owner_state.last_seq;
                    chunk.snapshot = true;
                    chunk.chunk = 0;
                    chunk.last = true;
                    auto frame = net::encode_sync_data_frame(chunk);
                    if (frame) session->send_frame(std::move(frame.value()));
                } else {
                    for (std::size_t c = 0; c < total_chunks; ++c) {
                        net::SyncDataMessage chunk;
                        chunk.id = req.id;
                        chunk.owner = owner_id;
                        chunk.epoch = owner_state.epoch;
                        chunk.from_seq = 0;
                        chunk.to_seq = owner_state.last_seq;
                        chunk.snapshot = true;
                        chunk.chunk = static_cast<std::uint32_t>(c);
                        chunk.last = (c == total_chunks - 1);

                        const std::size_t start = c * kChunkSize;
                        const std::size_t count = std::min(kChunkSize, total_ops - start);
                        for (std::size_t i = 0; i < count; ++i) {
                            chunk.ops.push_back(all_ops[start + i]);
                        }
                        auto frame = net::encode_sync_data_frame(chunk);
                    if (frame) session->send_frame(std::move(frame.value()));
                    }
                }
            }
        }
    }

    // Protocol §6.4: Send SYNC_END{id}
    net::SyncEndMessage end;
    end.id = req.id;
    auto end_frame = net::encode_sync_end_frame(end);
    if (end_frame) session->send_frame(std::move(end_frame.value()));
}

void SyncManager::handle_sync_data(std::shared_ptr<net::Session> session,
                                   const net::SyncDataMessage& data) {
    if (data.owner == options_.self_id) return;

    if (!data.snapshot) {
        // Delta
        auto res = registry_.apply_ops(data.owner, data.epoch, data.to_seq, data.ops);
        if (!res) return;

        for (const auto& op : data.ops) {
            if (op.op == "put") {
                PeerRecordRow row;
                row.owner_id = data.owner;
                row.name = op.name;
                row.type = op.type;
                row.ttl = op.ttl;
                row.data = op.data;
                row.created_at = op.created;
                (void)store_.put_peer_record(row);
            } else if (op.op == "del") {
                (void)store_.delete_peer_record(data.owner, op.name, op.type);
            }
            if (cache_invalidator_) cache_invalidator_(op.name);
        }

        process_buffered_updates(data.owner, session);
    } else {
        // Snapshot chunk
        std::vector<net::Op> full_snapshot;
        bool is_final = false;

        {
            std::lock_guard lock(mutex_);
            auto& buf = incoming_snapshots_[{session.get(), data.owner}];
            buf.insert(buf.end(), data.ops.begin(), data.ops.end());

            if (data.last) {
                full_snapshot = std::move(buf);
                incoming_snapshots_.erase({session.get(), data.owner});
                is_final = true;
            }
        }

        if (is_final) {
            auto res = registry_.reset_owner_snapshot(data.owner, data.epoch, data.to_seq, full_snapshot);
            if (res) {
                std::vector<PeerRecordRow> rows;
                rows.reserve(full_snapshot.size());
                for (const auto& op : full_snapshot) {
                    PeerRecordRow row;
                    row.owner_id = data.owner;
                    row.name = op.name;
                    row.type = op.type;
                    row.ttl = op.ttl;
                    row.data = op.data;
                    row.created_at = op.created;
                    rows.push_back(std::move(row));
                    if (cache_invalidator_) cache_invalidator_(op.name);
                }
                (void)store_.replace_peer_records(data.owner, rows);
                process_buffered_updates(data.owner, session);
            }
        }
    }
}

void SyncManager::handle_sync_end(std::shared_ptr<net::Session> /*session*/,
                                  const net::SyncEndMessage& /*end*/) {
    // Protocol §6.4: SYNC_END marks completion of SYNC_REQ response
}

void SyncManager::handle_update(std::shared_ptr<net::Session> session,
                               const net::UpdateMessage& update) {
    if (update.owner == options_.self_id) return;

    auto snap = registry_.snapshot();
    const auto* known = snap->find_owner(update.owner);

    if (!known || known->epoch != update.epoch) {
        // Unknown owner or different epoch -> request snapshot
        send_sync_req(session, update.owner);
        return;
    }

    if (update.seq <= known->last_seq) {
        // Duplicate or old sequence -> drop and do not relay
        return;
    }

    if (update.seq == known->last_seq + 1) {
        // Valid next sequence: apply atomically
        auto res = registry_.apply_ops(update.owner, update.epoch, update.seq, update.ops);
        if (!res) return;

        for (const auto& op : update.ops) {
            if (op.op == "put") {
                PeerRecordRow row;
                row.owner_id = update.owner;
                row.name = op.name;
                row.type = op.type;
                row.ttl = op.ttl;
                row.data = op.data;
                row.created_at = op.created;
                (void)store_.put_peer_record(row);
            } else if (op.op == "del") {
                (void)store_.delete_peer_record(update.owner, op.name, op.type);
            }
            if (cache_invalidator_) cache_invalidator_(op.name);
        }

        // Protocol §7.4: Relay update verbatim to all connected peers except sender and owner
        if (peer_manager_) {
            const auto sender_id = session ? session->peer_node_id() : NodeId{};
            auto frame = net::encode_update_frame(update);
            if (frame) peer_manager_->broadcast_frame(std::move(frame.value()), &sender_id, &update.owner);
        }

        process_buffered_updates(update.owner, session);
    } else {
        // Sequence gap: update.seq > known->last_seq + 1
        {
            std::lock_guard lock(mutex_);
            auto& buf = update_buffers_[update.owner];
            if (buf.size() < 100) {
                buf.push_back({update, std::chrono::steady_clock::now()});
            }
        }
        send_sync_req(session, update.owner);
    }
}

void SyncManager::process_buffered_updates(const NodeId& owner_id,
                                           std::shared_ptr<net::Session> sender) {
    while (true) {
        auto snap = registry_.snapshot();
        const auto* known = snap->find_owner(owner_id);
        if (!known) break;

        const uint64_t target_seq = known->last_seq + 1;
        std::optional<net::UpdateMessage> next_update;

        {
            std::lock_guard lock(mutex_);
            auto it = update_buffers_.find(owner_id);
            if (it == update_buffers_.end() || it->second.empty()) break;

            auto match_it = std::find_if(it->second.begin(), it->second.end(),
                                         [target_seq, epoch = known->epoch](const BufferedUpdate& bu) {
                                             return bu.update.seq == target_seq &&
                                                    bu.update.epoch == epoch;
                                         });

            if (match_it != it->second.end()) {
                next_update = match_it->update;
                it->second.erase(match_it);
            }
        }

        if (!next_update) break;

        auto res = registry_.apply_ops(owner_id, next_update->epoch, next_update->seq, next_update->ops);
        if (!res) break;

        for (const auto& op : next_update->ops) {
            if (op.op == "put") {
                PeerRecordRow row;
                row.owner_id = owner_id;
                row.name = op.name;
                row.type = op.type;
                row.ttl = op.ttl;
                row.data = op.data;
                row.created_at = op.created;
                (void)store_.put_peer_record(row);
            } else if (op.op == "del") {
                (void)store_.delete_peer_record(owner_id, op.name, op.type);
            }
            if (cache_invalidator_) cache_invalidator_(op.name);
        }

        if (peer_manager_) {
            const auto sender_id = sender ? sender->peer_node_id() : NodeId{};
            auto frame = net::encode_update_frame(*next_update);
            if (frame) peer_manager_->broadcast_frame(std::move(frame.value()), &sender_id, &owner_id);
        }
    }
}

Result<void> SyncManager::broadcast_local_update(const std::vector<net::Op>& ops) {
    if (ops.empty()) return {};

    auto ident_res = store_.identity();
    if (!ident_res) return ident_res.error();

    const auto current_epoch = ident_res.value().epoch;
    const auto current_seq = ident_res.value().seq;

    // Apply to local registry under self_id
    auto app_res = registry_.apply_ops(options_.self_id, current_epoch, current_seq, ops);
    if (!app_res) return app_res.error();

    for (const auto& op : ops) {
        if (cache_invalidator_) cache_invalidator_(op.name);
    }

    if (!peer_manager_) return {};

    if (ops.size() <= 1000) {
        net::UpdateMessage upd;
        upd.owner = options_.self_id;
        upd.epoch = current_epoch;
        upd.seq = current_seq;
        upd.ops = ops;
        auto frame = net::encode_update_frame(upd);
        if (frame) peer_manager_->broadcast_frame(std::move(frame.value()));
    } else {
        // Large transaction: broadcast proactive snapshot in chunks
        constexpr std::size_t kChunkSize = 1000;
        const std::size_t total_ops = ops.size();
        const std::size_t total_chunks = (total_ops + kChunkSize - 1) / kChunkSize;

        for (std::size_t c = 0; c < total_chunks; ++c) {
            net::SyncDataMessage chunk;
            chunk.id = 0;  // Proactive snapshot
            chunk.owner = options_.self_id;
            chunk.epoch = current_epoch;
            chunk.from_seq = 0;
            chunk.to_seq = current_seq;
            chunk.snapshot = true;
            chunk.chunk = static_cast<std::uint32_t>(c);
            chunk.last = (c == total_chunks - 1);

            const std::size_t start = c * kChunkSize;
            const std::size_t count = std::min(kChunkSize, total_ops - start);
            for (std::size_t i = 0; i < count; ++i) {
                chunk.ops.push_back(ops[start + i]);
            }
            auto frame = net::encode_sync_data_frame(chunk);
            if (frame) peer_manager_->broadcast_frame(std::move(frame.value()));
        }
    }

    return {};
}

void SyncManager::purge_offline(std::int64_t now_ts) {
    if (now_ts == 0) now_ts = std::time(nullptr);

    auto purged_ids = registry_.purge_offline_owners(options_.offline_retention, now_ts);
    for (const auto& pid : purged_ids) {
        (void)store_.delete_peer_records_for_owner(pid);
    }
}

void SyncManager::request_sync_for_owner(const NodeId& owner_id) {
    if (!peer_manager_) return;
    auto peers = peer_manager_->list_peers();
    for (const auto& p : peers) {
        if (p.is_online && p.session) {
            send_sync_req(p.session, owner_id);
            break;
        }
    }
}

}  // namespace ldns
