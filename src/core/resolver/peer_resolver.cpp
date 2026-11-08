#include "core/resolver/peer_resolver.hpp"

#include <algorithm>
#include <chrono>

namespace ldns::resolver {

PeerResolver::PeerResolver(std::shared_ptr<Registry> registry,
                           std::shared_ptr<net::PeerManager> peer_manager,
                           const dns::LocalZone& local_zone,
                           PeerResolverConfig config,
                           std::uint64_t self_epoch,
                           SeqProvider seq_provider)
    : registry_(std::move(registry)),
      peer_manager_(std::move(peer_manager)),
      local_zone_(local_zone),
      config_(config),
      self_epoch_(self_epoch),
      seq_provider_(std::move(seq_provider)) {}

PeerResolver::~PeerResolver() {
    std::lock_guard lock(mutex_);
    for (auto& [_, pending] : pending_queries_) {
        if (pending) {
            std::lock_guard plock(pending->mtx);
            pending->done = true;
            pending->cv.notify_all();
        }
    }
    pending_queries_.clear();
}

void PeerResolver::set_sync_trigger(SyncTriggerCallback cb) {
    sync_trigger_ = std::move(cb);
}

void PeerResolver::set_conflict_callback(ConflictCallback cb) {
    conflict_cb_ = std::move(cb);
}

void PeerResolver::set_config(PeerResolverConfig config) {
    std::lock_guard lock(mutex_);
    config_ = config;
}

PeerResolverConfig PeerResolver::config() const {
    std::lock_guard lock(mutex_);
    return config_;
}

void PeerResolver::set_self_epoch(std::uint64_t epoch) {
    self_epoch_ = epoch;
}

void PeerResolver::set_peer_manager(std::shared_ptr<net::PeerManager> pm) {
    std::lock_guard lock(mutex_);
    peer_manager_ = std::move(pm);
}

std::optional<dns::Message> PeerResolver::resolve(
    const dns::Name& name, std::uint16_t type, std::vector<TraceHop>& trace) {
    if (!registry_) return std::nullopt;

    auto snapshot = registry_->snapshot();
    if (!snapshot) return std::nullopt;

    PeerResolverConfig current_config;
    {
        std::lock_guard lock(mutex_);
        current_config = config_;
    }

    // Check local zone canonical data for conflict divergence detection
    std::vector<std::vector<std::uint8_t>> local_canon_data;
    const auto local_outcome = local_zone_.lookup(name, type);
    const bool has_local = (local_outcome.kind == dns::LookupKind::Answer);
    if (has_local) {
        for (const auto& rr : local_outcome.answers) {
            local_canon_data.push_back(rr.rdata);
        }
        local_canon_data = canonicalize_rrset_data(std::move(local_canon_data));
    }

    // Fast path: no network if no candidate claims in registry (SPEC §6.3.1)
    auto ranking = rank_peer_claims(
        *snapshot, name, type, true, {}, has_local ? &local_canon_data : nullptr);

    if (ranking.groups.empty()) {
        if (current_config.serve_stale) {
            auto stale_ranking = rank_peer_claims(
                *snapshot, name, type, false, {}, has_local ? &local_canon_data : nullptr);
            if (!stale_ranking.groups.empty() && !stale_ranking.groups[0].candidates.empty()) {
                const auto& grp = stale_ranking.groups[0];
                dns::Message msg;
                msg.flags = dns::kQr | dns::kRa;
                dns::Question q{name, type, dns::kClassIn};
                msg.questions.push_back(q);
                for (const auto& d : grp.canonical_data) {
                    dns::ResourceRecord rr;
                    rr.name = name;
                    rr.type = grp.type;
                    rr.ttl = std::min(grp.ttl, current_config.serve_stale_ttl);
                    rr.rdata = d;
                    msg.answers.push_back(std::move(rr));
                }
                TraceHop hop;
                hop.step = ResolutionStep::Peer;
                hop.qname = name;
                hop.qtype = type;
                hop.responder = "registry_stale";
                hop.rcode = dns::Rcode::NoError;
                hop.note = "served stale from offline owner";
                trace.push_back(std::move(hop));
                return msg;
            }
        }
        return std::nullopt;
    }

    if (ranking.conflict) {
        ConflictRecord cr;
        cr.name = name;
        cr.type = type;
        cr.groups = ranking.conflict->groups;
        cr.local_divergence = ranking.conflict->local_divergence;
        cr.detected_at = std::chrono::system_clock::now();
        {
            std::lock_guard lock(mutex_);
            conflicts_[{name.to_string(), type}] = cr;
        }
        if (conflict_cb_) {
            conflict_cb_(cr);
        }
    }

    std::shared_ptr<net::PeerManager> pm;
    {
        std::lock_guard lock(mutex_);
        pm = peer_manager_;
    }
    if (!pm) {
        return std::nullopt;
    }

    const auto overall_deadline = std::chrono::steady_clock::now() + current_config.budget_ms;

    struct QueryCleanup {
        PeerResolver& resolver;
        std::vector<std::uint32_t> ids;
        ~QueryCleanup() {
            if (!ids.empty()) {
                std::lock_guard lock(resolver.mutex_);
                for (auto id : ids) {
                    resolver.pending_queries_.erase(id);
                }
            }
        }
    } cleanup{*this};

    // Iterate through ranked groups
    for (const auto& group : ranking.groups) {
        if (std::chrono::steady_clock::now() >= overall_deadline) break;

        std::vector<std::pair<PeerCandidate, std::shared_ptr<PendingQuery>>> active_queries;

        for (std::size_t i = 0; i < group.candidates.size(); ++i) {
            const auto& cand = group.candidates[i];
            const auto now = std::chrono::steady_clock::now();
            if (now >= overall_deadline) break;

            std::uint32_t req_id = 0;
            auto pending = std::make_shared<PendingQuery>();
            {
                std::lock_guard lock(mutex_);
                req_id = next_req_id_++;
                pending_queries_[req_id] = pending;
            }
            cleanup.ids.push_back(req_id);

            net::ResolveMessage req;
            req.id = req_id;
            req.name = name.to_string();
            req.type = type;

            auto frame = net::encode_resolve_frame(req);
            bool sent = false;
            if (frame) {
                sent = pm->send_to_peer(cand.node_id, std::move(frame.value()));
            }

            if (!sent) {
                std::lock_guard lock(mutex_);
                pending_queries_.erase(req_id);
                continue;
            }

            active_queries.push_back({cand, pending});

            // Wait on the current candidate up to hedge_ms or remaining budget
            auto remaining_budget = std::chrono::duration_cast<std::chrono::milliseconds>(
                overall_deadline - std::chrono::steady_clock::now());
            if (remaining_budget.count() <= 0) break;

            auto wait_time = std::min(current_config.hedge_ms, remaining_budget);

            std::unique_lock plock(pending->mtx);
            const bool done = pending->cv.wait_for(plock, wait_time, [&]() { return pending->done; });

            if (done && pending->response) {
                const auto& resp = *pending->response;
                if (resp.rcode == 0 && !resp.answers.empty()) {
                    dns::Message msg;
                    msg.flags = dns::kQr | dns::kRa;
                    dns::Question q{name, type, dns::kClassIn};
                    msg.questions.push_back(q);

                    for (const auto& a : resp.answers) {
                        auto ans_name = dns::Name::from_presentation(a.name).value_or(name);
                        for (const auto& d : a.data) {
                            dns::ResourceRecord rr;
                            rr.name = ans_name;
                            rr.type = a.type;
                            rr.ttl = std::min(a.ttl, current_config.peer_ttl_cap);
                            rr.rdata = d;
                            msg.answers.push_back(std::move(rr));
                        }
                    }

                    TraceHop hop;
                    hop.step = ResolutionStep::Peer;
                    hop.qname = name;
                    hop.qtype = type;
                    hop.responder = to_hex(cand.node_id);
                    hop.rcode = dns::Rcode::NoError;
                    hop.note = "peer resolved";
                    trace.push_back(std::move(hop));
                    return msg;
                } else if (resp.epoch > 0 || resp.seq > 0) {
                    if (sync_trigger_) {
                        sync_trigger_(cand.node_id);
                    }
                }
            }
        }

        // Wait for remaining in-flight queries of this group if any
        while (std::chrono::steady_clock::now() < overall_deadline) {
            bool any_pending = false;
            for (auto& [cand, pending] : active_queries) {
                std::unique_lock plock(pending->mtx);
                if (pending->done && pending->response) {
                    const auto& resp = *pending->response;
                    if (resp.rcode == 0 && !resp.answers.empty()) {
                        dns::Message msg;
                        msg.flags = dns::kQr | dns::kRa;
                        dns::Question q{name, type, dns::kClassIn};
                        msg.questions.push_back(q);

                        for (const auto& a : resp.answers) {
                            auto ans_name = dns::Name::from_presentation(a.name).value_or(name);
                            for (const auto& d : a.data) {
                                dns::ResourceRecord rr;
                                rr.name = ans_name;
                                rr.type = a.type;
                                rr.ttl = std::min(a.ttl, current_config.peer_ttl_cap);
                                rr.rdata = d;
                                msg.answers.push_back(std::move(rr));
                            }
                        }

                        TraceHop hop;
                        hop.step = ResolutionStep::Peer;
                        hop.qname = name;
                        hop.qtype = type;
                        hop.responder = to_hex(cand.node_id);
                        hop.rcode = dns::Rcode::NoError;
                        hop.note = "peer resolved (hedged)";
                        trace.push_back(std::move(hop));
                        return msg;
                    }
                } else if (!pending->done) {
                    any_pending = true;
                    pending->cv.wait_for(plock, std::chrono::milliseconds(20), [&]() { return pending->done; });
                }
            }
            if (!any_pending) break;
        }
    }

    // Offline serve_stale fallback after online candidates failure
    if (current_config.serve_stale) {
        auto stale_ranking = rank_peer_claims(
            *snapshot, name, type, false, {}, has_local ? &local_canon_data : nullptr);
        if (!stale_ranking.groups.empty() && !stale_ranking.groups[0].candidates.empty()) {
            const auto& grp = stale_ranking.groups[0];
            dns::Message msg;
            msg.flags = dns::kQr | dns::kRa;
            dns::Question q{name, type, dns::kClassIn};
            msg.questions.push_back(q);
            for (const auto& d : grp.canonical_data) {
                dns::ResourceRecord rr;
                rr.name = name;
                rr.type = grp.type;
                rr.ttl = std::min(grp.ttl, current_config.serve_stale_ttl);
                rr.rdata = d;
                msg.answers.push_back(std::move(rr));
            }
            TraceHop hop;
            hop.step = ResolutionStep::Peer;
            hop.qname = name;
            hop.qtype = type;
            hop.responder = "registry_stale";
            hop.rcode = dns::Rcode::NoError;
            hop.note = "served stale after online candidates failed";
            trace.push_back(std::move(hop));
            return msg;
        }
    }

    return std::nullopt;
}

void PeerResolver::handle_resolve_request(std::shared_ptr<net::Session> session,
                                          const net::ResolveMessage& req) {
    if (!session) return;

    net::ResolveResponseMessage resp;
    resp.id = req.id;
    resp.epoch = self_epoch_;
    resp.seq = seq_provider_ ? seq_provider_() : 0;

    auto parsed_name = dns::Name::from_presentation(req.name);
    if (!parsed_name) {
        resp.rcode = 1;  // FORMERR
        auto frame = net::encode_resolve_response_frame(resp);
        if (frame) session->send_frame(std::move(frame.value()));
        return;
    }

    // Step 8.2: Query ONLY LocalZone! Never forward or query upstream/registry.
    const auto outcome = local_zone_.lookup(parsed_name.value(), req.type);
    if (outcome.kind == dns::LookupKind::Answer) {
        resp.rcode = 0;
        net::ResolveAnswer ans;
        ans.name = req.name;
        ans.type = req.type;
        ans.ttl = 60;
        for (const auto& rr : outcome.answers) {
            ans.ttl = rr.ttl;
            ans.data.push_back(rr.rdata);
        }
        resp.answers.push_back(std::move(ans));
    } else if (outcome.kind == dns::LookupKind::Cname && outcome.cname_target) {
        resp.rcode = 0;
        net::ResolveAnswer ans;
        ans.name = req.name;
        ans.type = static_cast<std::uint16_t>(dns::RrType::CNAME);
        ans.ttl = outcome.cname_ttl != 0 ? outcome.cname_ttl : 60;
        auto wire = outcome.cname_target->wire();
        ans.data.push_back(std::vector<std::uint8_t>(wire.begin(), wire.end()));
        resp.answers.push_back(std::move(ans));
    } else if (outcome.kind == dns::LookupKind::NoData) {
        resp.rcode = 0;
        resp.answers = {};
    } else {
        resp.rcode = 3;  // NXDOMAIN
        resp.answers = {};
    }

    auto frame = net::encode_resolve_response_frame(resp);
    if (frame) {
        session->send_frame(std::move(frame.value()));
    }
}

void PeerResolver::handle_resolve_response(std::shared_ptr<net::Session>,
                                           const net::ResolveResponseMessage& resp) {
    std::shared_ptr<PendingQuery> pending;
    {
        std::lock_guard lock(mutex_);
        const auto it = pending_queries_.find(resp.id);
        if (it != pending_queries_.end()) {
            pending = it->second;
            pending_queries_.erase(it);
        }
    }
    if (pending) {
        std::lock_guard plock(pending->mtx);
        pending->response = resp;
        pending->done = true;
        pending->cv.notify_all();
    }
}

std::vector<ConflictRecord> PeerResolver::conflicts() const {
    std::lock_guard lock(mutex_);
    std::vector<ConflictRecord> list;
    list.reserve(conflicts_.size());
    for (const auto& [_, c] : conflicts_) {
        list.push_back(c);
    }
    return list;
}

std::optional<ConflictRecord> PeerResolver::get_conflict(const dns::Name& name,
                                                         std::uint16_t type) const {
    std::lock_guard lock(mutex_);
    const auto it = conflicts_.find({name.to_string(), type});
    if (it != conflicts_.end()) return it->second;
    return std::nullopt;
}

void PeerResolver::clear_conflicts() {
    std::lock_guard lock(mutex_);
    conflicts_.clear();
}

}  // namespace ldns::resolver
