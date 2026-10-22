#include "core/server/query_handler.hpp"

#include <algorithm>

namespace ldns {
namespace {

using dns::LookupKind;
using dns::Message;
using dns::Name;
using dns::ResourceRecord;
using dns::Rcode;
using dns::RrType;

constexpr int kMaxCnameChain = 8;
constexpr std::uint32_t kDefaultTtl = 60;

std::uint16_t type_of(RrType type) { return static_cast<std::uint16_t>(type); }

// Addresses a response even when the rest of the packet is malformed.
Message make_response_shell(std::span<const std::uint8_t> packet) {
    Message response;
    response.id = static_cast<std::uint16_t>((packet[0] << 8) | packet[1]);
    response.flags = dns::kQr;
    return response;
}

}  // namespace

QueryHandler::QueryHandler(const dns::LocalZone& zone, HandlerConfig config)
    : zone_(zone), config_(config) {}

std::optional<HandlerResponse> QueryHandler::handle(std::span<const std::uint8_t> packet,
                                                    const QueryContext& /*context*/) const {
    if (packet.size() < dns::kHeaderSize) return std::nullopt;

    // Drop responses sent to the server port (also a basic loop guard).
    const auto raw_flags = static_cast<std::uint16_t>((packet[2] << 8) | packet[3]);
    if ((raw_flags & dns::kQr) != 0) return std::nullopt;

    auto parsed = Message::parse(packet);
    if (!parsed) {
        // Header was long enough to answer: tell the client its packet is bad.
        Message response = make_response_shell(packet);
        response.set_rcode(Rcode::FormErr);
        return HandlerResponse{std::move(response), false};
    }

    const Message& query = parsed.value();
    Message response = make_response_shell(packet);
    response.flags = static_cast<std::uint16_t>(response.flags | (query.flags & dns::kRd));
    response.set_flag(dns::kRa, config_.recursive_available);

    if (query.opcode() != 0) {
        response.set_rcode(Rcode::NotImp);
        response.questions = query.questions;
        return HandlerResponse{std::move(response), false};
    }
    if (query.questions.size() != 1) {
        response.set_rcode(Rcode::FormErr);
        return HandlerResponse{std::move(response), false};
    }

    const dns::Question& question = query.questions.front();
    response.questions.push_back(question);

    if (question.qclass != dns::kClassIn) {
        response.set_rcode(Rcode::Refused);
        return HandlerResponse{std::move(response), false};
    }

    // EDNS0: echo an OPT with our payload size, capped by the client's.
    // An unsupported EDNS version gets BADVERS (extended rcode 16) with an
    // empty answer section (RFC 6891 §6.1.3).
    if (query.edns) {
        dns::EdnsInfo edns;
        edns.udp_payload = std::min(config_.edns_udp_payload, query.edns->udp_payload);
        edns.do_bit = query.edns->do_bit;
        if (query.edns->version != dns::kEdnsVersion) {
            edns.extended_rcode = 1;  // BADVERS: extended rcode 16 = 1 << 4 | 0
            response.edns = edns;
            response.set_flag(dns::kAa, false);
            return HandlerResponse{std::move(response), false};
        }
        response.edns = edns;
    }

    // Local zone lookup, following CNAMEs inside the zone (SPEC §6.3.4).
    // M1 has no upstream: a name the zone does not know is answered from the
    // zone's point of view (NXDOMAIN when the zone is non-empty, REFUSED when
    // it is empty). M3 replaces this tail with the upstream step.
    std::vector<ResourceRecord> answers;
    std::vector<Name> visited;
    Name current = question.name;
    bool name_exists = false;
    bool loop = false;
    // True when the loop ends immediately after following a CNAME, i.e. the
    // chain was cut short instead of terminating on an answer/NODATA/NXDOMAIN.
    bool last_was_cname = false;

    for (int step = 0; step < kMaxCnameChain; ++step) {
        visited.push_back(current);
        const auto outcome = zone_.lookup(current, question.type);

        if (outcome.kind == LookupKind::Answer) {
            answers.insert(answers.end(), outcome.answers.begin(), outcome.answers.end());
            name_exists = true;
            last_was_cname = false;
            break;
        }
        if (outcome.kind == LookupKind::Cname && outcome.cname_target) {
            ResourceRecord cname;
            cname.name = current;
            cname.type = type_of(RrType::CNAME);
            cname.ttl = kDefaultTtl;
            auto rdata = dns::encode_rdata_name(*outcome.cname_target);
            if (!rdata) break;
            cname.rdata = rdata.value();
            answers.push_back(std::move(cname));
            name_exists = true;

            const Name& target = *outcome.cname_target;
            if (std::find(visited.begin(), visited.end(), target) != visited.end()) {
                loop = true;
                break;
            }
            current = target;
            last_was_cname = true;
            continue;
        }
        if (outcome.kind == LookupKind::NoData) {
            // Name exists, type does not: NOERROR/NODATA with an SOA.
            name_exists = true;
            last_was_cname = false;
            break;
        }
        // NxDomain: the chain left the local zone. The CNAMEs we collected are
        // still a valid authoritative answer; in M3 the resolver continues
        // with the upstream step.
        last_was_cname = false;
        break;
    }

    // A CNAME loop, or running out of chain steps while still following
    // CNAMEs, is a SERVFAIL with the partial chain attached (SPEC §6.3.4).
    if (loop || last_was_cname) {
        response.set_rcode(Rcode::ServFail);
        response.answers = std::move(answers);
        response.set_flag(dns::kAa, true);
        return HandlerResponse{std::move(response), true};
    }

    if (!name_exists && answers.empty()) {
        if (zone_.empty()) {
            response.set_rcode(Rcode::Refused);
            return HandlerResponse{std::move(response), false};
        }
        response.set_rcode(Rcode::NxDomain);
        if (auto soa = zone_.negative_soa(question.name); soa) {
            response.authority.push_back(std::move(soa.value()));
        }
        response.set_flag(dns::kAa, true);
        return HandlerResponse{std::move(response), true};
    }

    // Negative answers (NODATA, and CNAME chains that end outside the zone)
    // carry the nearest SOA so clients can cache the negative result.
    if (answers.empty()) {
        if (auto soa = zone_.negative_soa(question.name); soa) {
            response.authority.push_back(std::move(soa.value()));
        }
    }

    response.set_flag(dns::kAa, true);
    response.answers = std::move(answers);
    return HandlerResponse{std::move(response), true};
}

}  // namespace ldns
