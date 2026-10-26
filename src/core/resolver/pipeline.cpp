#include "core/resolver/pipeline.hpp"

#include <algorithm>
#include <chrono>

namespace ldns::resolver {
namespace {

using dns::LookupKind;
using dns::Message;
using dns::Name;
using dns::Rcode;
using dns::ResourceRecord;
using dns::RrType;

std::string rcode_string(std::uint8_t code) {
    switch (static_cast<Rcode>(code)) {
        case Rcode::NoError: return "NOERROR";
        case Rcode::FormErr: return "FORMERR";
        case Rcode::ServFail: return "SERVFAIL";
        case Rcode::NxDomain: return "NXDOMAIN";
        case Rcode::NotImp: return "NOTIMP";
        case Rcode::Refused: return "REFUSED";
        default: return "RCODE" + std::to_string(code);
    }
}

std::uint64_t now_ms() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

}  // namespace

ResolverPipeline::ResolverPipeline(const dns::LocalZone& local_zone,
                                   std::shared_ptr<DnsCache> cache,
                                   std::shared_ptr<UpstreamClient> upstream,
                                   std::shared_ptr<QueryLog> query_log,
                                   PipelineConfig config)
    : local_zone_(local_zone),
      cache_(std::move(cache)),
      upstream_(std::move(upstream)),
      query_log_(std::move(query_log)),
      config_(config) {}

void ResolverPipeline::set_peer_hook(PeerResolverHook hook) {
    peer_hook_ = std::move(hook);
}

void ResolverPipeline::set_config(PipelineConfig config) {
    config_ = config;
}

PipelineResult ResolverPipeline::resolve(const dns::Message& query,
                                         const QueryContext& context,
                                         bool enable_trace) {
    const auto start_time = std::chrono::steady_clock::now();
    PipelineResult result;

    result.response.id = query.id;
    result.response.flags = dns::kQr;
    result.response.flags = static_cast<std::uint16_t>(
        result.response.flags | (query.flags & dns::kRd));
    result.response.set_flag(dns::kRa, true);  // RA = 1 per SPEC §6.3.7

    if (query.opcode() != 0) {
        result.response.set_rcode(Rcode::NotImp);
        result.response.questions = query.questions;
        return result;
    }
    if (query.questions.size() != 1) {
        result.response.set_rcode(Rcode::FormErr);
        return result;
    }

    const dns::Question& question = query.questions.front();
    result.response.questions.push_back(question);

    if (question.qclass != dns::kClassIn) {
        result.response.set_rcode(Rcode::Refused);
        return result;
    }

    // EDNS0 negotiation
    if (query.edns) {
        dns::EdnsInfo edns;
        edns.udp_payload = std::min<std::uint16_t>(1232, query.edns->udp_payload);
        edns.do_bit = query.edns->do_bit;
        if (query.edns->version != dns::kEdnsVersion) {
            edns.extended_rcode = 1;  // BADVERS
            result.response.edns = edns;
            result.response.set_flag(dns::kAa, false);
            return result;
        }
        result.response.edns = edns;
    }

    std::vector<ResourceRecord> collected_answers;
    std::vector<Name> visited;
    Name current = question.name;
    bool answered = false;
    bool loop = false;
    bool last_was_cname = false;
    ResolutionStep last_step = ResolutionStep::None;
    std::string responder_info;

    const auto record_trace = [&](ResolutionStep step, const Name& name,
                                  std::string responder, dns::Rcode rcode,
                                  std::string note) {
        if (!enable_trace) return;
        const auto now = std::chrono::steady_clock::now();
        const auto lat = std::chrono::duration_cast<std::chrono::microseconds>(
                             now - start_time)
                             .count();
        result.trace.push_back(TraceHop{step, name, question.type,
                                        std::move(responder),
                                        static_cast<std::uint64_t>(lat), rcode,
                                        std::move(note)});
    };

    for (int hop = 0; hop < config_.max_cname_chain; ++hop) {
        visited.push_back(current);

        // --- Step 0: Cache -------------------------------------------------
        if (cache_) {
            auto cached = cache_->get(current, question.type, question.qclass);
            if (cached) {
                last_step = ResolutionStep::Cache;
                responder_info = "cache";
                record_trace(ResolutionStep::Cache, current, "cache", cached->rcode,
                             cached->is_negative ? "cache negative hit" : "cache hit");

                if (cached->is_negative) {
                    result.response.set_rcode(cached->rcode);
                    auto auth = cached->adjusted_authority(std::chrono::steady_clock::now());
                    result.response.authority.insert(result.response.authority.end(),
                                                     auth.begin(), auth.end());
                    answered = true;
                    last_was_cname = false;
                    break;
                }

                auto adj_answers = cached->adjusted_answers(std::chrono::steady_clock::now());
                collected_answers.insert(collected_answers.end(), adj_answers.begin(),
                                         adj_answers.end());

                // Check if the cached entry was a CNAME
                bool found_cname = false;
                for (const auto& rr : adj_answers) {
                    if (rr.type == static_cast<std::uint16_t>(RrType::CNAME)) {
                        std::size_t offset = 0;
                        auto target = Name::from_wire(rr.rdata, offset);
                        if (target) {
                            if (std::find(visited.begin(), visited.end(), target.value()) !=
                                visited.end()) {
                                loop = true;
                                break;
                            }
                            current = target.value();
                            found_cname = true;
                            last_was_cname = true;
                            break;
                        }
                    }
                }
                if (loop) break;
                if (found_cname) continue;

                answered = true;
                last_was_cname = false;
                break;
            }
        }

        // --- Step 1: LocalZone ---------------------------------------------
        const auto local_outcome = local_zone_.lookup(current, question.type);
        if (local_outcome.kind == LookupKind::Answer) {
            last_step = ResolutionStep::Local;
            result.response.set_flag(dns::kAa, true);
            collected_answers.insert(collected_answers.end(),
                                     local_outcome.answers.begin(),
                                     local_outcome.answers.end());
            record_trace(ResolutionStep::Local, current, "local_zone", Rcode::NoError,
                         "local authoritative answer");
            answered = true;
            last_was_cname = false;
            break;
        }

        if (local_outcome.kind == LookupKind::Cname && local_outcome.cname_target) {
            last_step = ResolutionStep::Local;
            result.response.set_flag(dns::kAa, true);
            ResourceRecord cname_rr;
            cname_rr.name = current;
            cname_rr.type = static_cast<std::uint16_t>(RrType::CNAME);
            cname_rr.ttl = local_outcome.cname_ttl != 0 ? local_outcome.cname_ttl : 60;
            auto rdata = dns::encode_rdata_name(*local_outcome.cname_target);
            if (!rdata) break;
            cname_rr.rdata = std::move(rdata.value());
            collected_answers.push_back(std::move(cname_rr));

            record_trace(ResolutionStep::Local, current, "local_zone", Rcode::NoError,
                         "local CNAME to " + local_outcome.cname_target->to_string());

            const Name& target = *local_outcome.cname_target;
            if (std::find(visited.begin(), visited.end(), target) != visited.end()) {
                loop = true;
                break;
            }
            current = target;
            last_was_cname = true;
            continue;
        }

        if (local_outcome.kind == LookupKind::NoData) {
            // Name exists in LocalZone, but type does NOT. SPEC §6.2: stop!
            last_step = ResolutionStep::Local;
            result.response.set_flag(dns::kAa, true);
            result.response.set_rcode(Rcode::NoError);
            if (auto soa = local_zone_.negative_soa(current); soa) {
                result.response.authority.push_back(std::move(soa.value()));
            }
            record_trace(ResolutionStep::Local, current, "local_zone", Rcode::NoError,
                         "local NODATA (name exists, type does not)");
            answered = true;
            last_was_cname = false;
            break;
        }

        // --- Step 2: PeerResolver (hook) -----------------------------------
        if (peer_hook_) {
            auto peer_res = peer_hook_(current, question.type, result.trace);
            if (peer_res) {
                last_step = ResolutionStep::Peer;
                result.response.set_flag(dns::kAa, false);
                result.response.set_rcode(static_cast<Rcode>(peer_res->rcode()));
                collected_answers.insert(collected_answers.end(),
                                         peer_res->answers.begin(),
                                         peer_res->answers.end());
                result.response.authority.insert(result.response.authority.end(),
                                                 peer_res->authority.begin(),
                                                 peer_res->authority.end());

                if (cache_ && !peer_res->answers.empty()) {
                    cache_->put_positive(current, question.type,
                                         peer_res->answers,
                                         peer_res->authority,
                                         CacheSource::Peer,
                                         config_.peer_ttl_cap,
                                         question.qclass);
                }

                // Check for CNAME in peer response (SPEC §6.3.4)
                bool found_cname = false;
                for (const auto& rr : peer_res->answers) {
                    if (rr.type == static_cast<std::uint16_t>(RrType::CNAME)) {
                        std::size_t offset = 0;
                        auto target = Name::from_wire(rr.rdata, offset);
                        if (target) {
                            if (std::find(visited.begin(), visited.end(), target.value()) !=
                                visited.end()) {
                                loop = true;
                                break;
                            }
                            current = target.value();
                            found_cname = true;
                            last_was_cname = true;
                            break;
                        }
                    }
                }
                if (loop) break;
                if (found_cname) continue;

                answered = true;
                last_was_cname = false;
                break;
            }
        }

        // --- Step 3: Upstream ----------------------------------------------
        if (upstream_) {
            Message upstream_query;
            upstream_query.id = query.id;
            upstream_query.flags = query.flags & dns::kRd;  // pass recursion desired
            upstream_query.questions.push_back(
                dns::Question{current, question.type, question.qclass});
            if (query.edns) upstream_query.edns = query.edns;

            auto up_res = upstream_->resolve(upstream_query);
            if (up_res) {
                const auto& resp = up_res.value();
                last_step = ResolutionStep::Upstream;
                responder_info = resp.responder;
                result.response.set_flag(dns::kAa, false);
                result.response.set_rcode(
                    static_cast<Rcode>(resp.message.rcode()));

                collected_answers.insert(collected_answers.end(),
                                         resp.message.answers.begin(),
                                         resp.message.answers.end());
                result.response.authority.insert(result.response.authority.end(),
                                                 resp.message.authority.begin(),
                                                 resp.message.authority.end());

                record_trace(ResolutionStep::Upstream, current, resp.responder,
                             static_cast<Rcode>(resp.message.rcode()),
                             "upstream response in " +
                                 std::to_string(resp.latency_us) + "us");

                // Cache the response if cache is enabled
                if (cache_) {
                    if (resp.message.rcode() == static_cast<std::uint8_t>(Rcode::NoError)) {
                        if (!resp.message.answers.empty()) {
                            cache_->put_positive(current, question.type,
                                                 resp.message.answers,
                                                 resp.message.authority,
                                                 CacheSource::Upstream,
                                                 config_.upstream_ttl_cap,
                                                 question.qclass);
                        } else {
                            // Upstream NODATA
                            std::optional<ResourceRecord> soa;
                            for (const auto& a : resp.message.authority) {
                                if (a.type == static_cast<std::uint16_t>(RrType::SOA)) {
                                    soa = a;
                                    break;
                                }
                            }
                            cache_->put_negative(current, question.type,
                                                 Rcode::NoError, soa,
                                                 CacheSource::Upstream,
                                                 config_.negative_ttl,
                                                 question.qclass);
                        }
                    } else if (resp.message.rcode() ==
                               static_cast<std::uint8_t>(Rcode::NxDomain)) {
                        std::optional<ResourceRecord> soa;
                        for (const auto& a : resp.message.authority) {
                            if (a.type == static_cast<std::uint16_t>(RrType::SOA)) {
                                soa = a;
                                break;
                            }
                        }
                        cache_->put_negative(current, question.type,
                                             Rcode::NxDomain, soa,
                                             CacheSource::Upstream,
                                             config_.negative_ttl,
                                             question.qclass);
                    }
                }

                answered = true;
                last_was_cname = false;
                break;
            } else {
                record_trace(ResolutionStep::Upstream, current, "all_upstreams",
                             Rcode::ServFail, up_res.error().message);
            }
        }

        // Neither local nor upstream could resolve
        last_was_cname = false;
        break;
    }

    if (loop || last_was_cname) {
        result.response.set_rcode(Rcode::ServFail);
    } else if (!answered) {
        if (local_zone_.empty() && (!upstream_ || upstream_->config().servers.empty())) {
            result.response.set_rcode(Rcode::Refused);
        } else {
            result.response.set_rcode(Rcode::NxDomain);
            if (auto soa = local_zone_.negative_soa(question.name); soa) {
                result.response.authority.push_back(std::move(soa.value()));
            }
        }
    }

    result.response.answers = std::move(collected_answers);
    result.resolved_step = last_step;
    result.from_cache = (last_step == ResolutionStep::Cache);

    const auto end_time = std::chrono::steady_clock::now();
    result.total_latency_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time)
            .count());

    // Record into QueryLog
    if (query_log_) {
        QueryLogEntry entry;
        entry.timestamp_ms = now_ms();
        entry.client_ip = context.client_ip;
        entry.qname = question.name.to_string();
        entry.qtype = dns::rr_type_name(question.type);
        entry.step = last_step;
        entry.rcode = rcode_string(result.response.rcode());
        entry.latency_us = result.total_latency_us;
        entry.responder = responder_info;
        query_log_->record(std::move(entry));
    }

    return result;
}

}  // namespace ldns::resolver
