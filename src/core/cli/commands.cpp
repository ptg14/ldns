#include "core/cli/commands.hpp"

#include <fstream>
#include <iostream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "core/domain/import_export.hpp"

namespace ldns::cli {
namespace {

using json = nlohmann::json;

constexpr std::uint32_t kMaxListPage = 100'000;

void print_json(const json& document) {
    std::cout << document.dump(2) << '\n';
}

json entry_json(const domain::DomainEntry& entry) {
    json out;
    out["name"] = entry.name;
    out["type"] = entry.type;
    out["ttl"] = entry.ttl;
    out["target_kind"] = entry.target_kind;
    out["targets"] = entry.targets;
    out["created"] = entry.created;
    out["updated"] = entry.updated;
    return out;
}

void print_entries_human(const std::vector<domain::DomainEntry>& entries) {
    if (entries.empty()) {
        std::cout << "no domains\n";
        return;
    }
    for (const auto& entry : entries) {
        std::cout << entry.name << "  " << entry.type << "  " << entry.ttl << "s  "
                  << entry.target_kind << "  ";
        for (std::size_t i = 0; i < entry.targets.size(); ++i) {
            if (i != 0) std::cout << ", ";
            std::cout << entry.targets[i];
        }
        std::cout << '\n';
    }
}

void print_entry_human(const domain::DomainEntry& entry) {
    std::cout << entry.name << " -> ";
    for (std::size_t i = 0; i < entry.targets.size(); ++i) {
        if (i != 0) std::cout << ", ";
        std::cout << entry.targets[i];
    }
    std::cout << " (" << entry.type << ", " << entry.target_kind << ", " << entry.ttl << "s)\n";
}

}  // namespace

int exit_code_for(const Error& error) {
    switch (error.code) {
        case 703:  // already exists
        case 705:  // needs --force
            return kExitConflict;
        case 700:
        case 701:
        case 702:
            return kExitUsage;
        default:
            return kExitError;
    }
}

Result<domain::DomainEntry> add(CoreApi& api, const AddOptions& options) {
    domain::AddRequest request;
    request.name = options.name;
    request.type = options.type;
    request.ttl = options.ttl;
    request.force = options.force;
    request.kind = domain::TargetKind::Static;

    if (options.targets.empty()) {
        // SPEC §9.1: the default target is `self`.
        auto added = api.add_self_domain(options.name, options.type, options.ttl);
        if (!added) return added.error();
        if (options.json) print_json(entry_json(added.value()));
        else print_entry_human(added.value());
        return added;
    }

    request.targets = options.targets;
    auto added = api.add_domain(std::move(request));
    if (!added) return added.error();
    if (options.json) print_json(entry_json(added.value()));
    else print_entry_human(added.value());
    return added;
}

Result<domain::DomainEntry> set(CoreApi& api, const AddOptions& options) {
    if (options.targets.empty()) {
        return Error{702, "set needs at least one target (use `self` or an address)"};
    }
    domain::AddRequest request;
    request.name = options.name;
    request.type = options.type;
    request.ttl = options.ttl;
    request.force = options.force;
    request.targets = options.targets;
    request.kind = domain::TargetKind::Static;

    auto updated = api.set_domain(std::move(request));
    if (!updated) return updated.error();
    if (options.json) print_json(entry_json(updated.value()));
    else print_entry_human(updated.value());
    return updated;
}

Result<void> remove(CoreApi& api, std::string_view name, std::string_view type) {
    auto status = api.remove_domain(name, type);
    if (!status) return status.error();
    return {};
}

Result<std::vector<domain::DomainEntry>> list(CoreApi& api, const ListOptions& options) {
    auto entries = api.list_domains(options.type);
    if (!entries) return entries.error();
    if (options.json) {
        json document = json::array();
        for (const auto& entry : entries.value()) document.push_back(entry_json(entry));
        print_json(document);
    } else {
        print_entries_human(entries.value());
    }
    return entries;
}

Result<std::size_t> status(CoreApi& api, bool json_output) {
    const NodeStatus status = api.status();
    const config::Config& config = api.config();

    if (json_output) {
        json document;
        document["node_id"] = to_hex(status.node_id);
        document["short_id"] = status.short_id;
        document["epoch"] = status.epoch;
        document["seq"] = status.seq;
        document["network"] = status.network_name;
        document["config_file"] = status.config_file;
        document["data_file"] = status.data_file;
        document["domains"] = status.domain_count;
        document["self_addresses"] = status.self_addresses;
        document["psk_configured"] = status.psk_configured;
        document["psk_key_file_found"] = status.psk_key_file_found;
        document["listen"] = config.server.listen;
        std::vector<std::string> warnings;
        if (!status.psk_configured) {
            warnings.push_back(
                "no network key set: anyone on the LAN can join this Ldns network (SPEC D16)");
        } else if (!status.psk_key_file_found) {
            warnings.push_back("network.key_file is set but the file is missing");
        }
        if (status.self_addresses.empty()) {
            warnings.push_back("no LAN address found for `self` records; set network.self_ip");
        }
        document["warnings"] = warnings;
        print_json(document);
        return status.domain_count;
    }

    std::cout << "node:    " << status.short_id << "  (" << to_hex(status.node_id) << ")\n";
    std::cout << "network: " << status.network_name << "  epoch " << status.epoch << "  seq "
              << status.seq << '\n';
    std::cout << "db:      " << status.data_file << '\n';
    std::cout << "config:  " << status.config_file << '\n';
    std::cout << "domains: " << status.domain_count << '\n';
    std::cout << "self:    ";
    if (status.self_addresses.empty()) {
        std::cout << "(none)";
    } else {
        for (std::size_t i = 0; i < status.self_addresses.size(); ++i) {
            if (i != 0) std::cout << ", ";
            std::cout << status.self_addresses[i];
        }
    }
    std::cout << '\n';
    if (!status.psk_configured) {
        std::cout << "warning: no network key set: anyone on the LAN can join this Ldns network\n";
    } else if (!status.psk_key_file_found) {
        std::cout << "warning: network.key_file is set but the file is missing\n";
    }
    if (status.self_addresses.empty()) {
        std::cout << "warning: no LAN address found for `self` records; set network.self_ip\n";
    }
    return status.domain_count;
}

Result<domain::ImportReport> import_file(CoreApi& api, const ImportOptions& options) {
    auto report = api.import_file(options.path, options.force);
    if (!report) return report.error();
    if (options.json) {
        json document;
        document["imported"] = report.value().imported;
        document["skipped"] = report.value().skipped;
        document["warnings"] = report.value().warnings;
        print_json(document);
    } else {
        std::cout << "imported " << report.value().imported << " record(s)";
        if (report.value().skipped != 0) std::cout << ", skipped " << report.value().skipped;
        std::cout << '\n';
        for (const auto& warning : report.value().warnings) {
            std::cout << "warning: " << warning << '\n';
        }
    }
    return report;
}

Result<void> export_zone(CoreApi& api, const ExportOptions& options) {
    auto text = api.export_zone(!options.csv);
    if (!text) return text.error();

    if (options.json) {
        auto entries = api.list_domains(std::nullopt);
        if (!entries) return entries.error();
        json document = json::array();
        for (const auto& entry : entries.value()) document.push_back(entry_json(entry));
        print_json(document);
        return {};
    }

    if (options.path) {
        std::ofstream file(*options.path, std::ios::binary);
        if (!file) return Error{731, "cannot write " + *options.path};
        file << text.value();
        if (!file) return Error{731, "write failed: " + *options.path};
        std::cout << "wrote " << options.path->size() << " bytes to " << *options.path << '\n';
        return {};
    }
    std::cout << text.value();
    return {};
}

std::string format_rdata(std::uint16_t type, std::span<const std::uint8_t> rdata) {
    return dns::format_rdata(type, rdata);
}

Result<resolver::PipelineResult> resolve(CoreApi& api, const ResolveOptions& options,
                                         std::ostream& out) {
    auto res = api.resolve(options.name, options.type, options.trace);
    if (!res) return res.error();

    const auto& result = res.value();
    if (options.json) {
        json doc;
        doc["qname"] = options.name;
        doc["qtype"] = options.type;
        doc["rcode"] = dns::rcode_name(static_cast<dns::Rcode>(result.response.rcode()));
        doc["step"] = resolver::step_name(result.resolved_step);
        doc["total_latency_us"] = result.total_latency_us;
        doc["from_cache"] = result.from_cache;

        json answers = json::array();
        for (const auto& a : result.response.answers) {
            json item;
            item["name"] = a.name.to_string();
            item["type"] = std::string(dns::rr_type_name(a.type));
            item["ttl"] = a.ttl;
            item["data"] = format_rdata(a.type, a.rdata);
            answers.push_back(item);
        }
        doc["answers"] = answers;

        json authority = json::array();
        for (const auto& a : result.response.authority) {
            json item;
            item["name"] = a.name.to_string();
            item["type"] = std::string(dns::rr_type_name(a.type));
            item["ttl"] = a.ttl;
            item["data"] = format_rdata(a.type, a.rdata);
            authority.push_back(item);
        }
        doc["authority"] = authority;

        if (options.trace) {
            json trace_arr = json::array();
            for (const auto& h : result.trace) {
                json hop;
                hop["step"] = resolver::step_name(h.step);
                hop["qname"] = h.qname.to_string();
                hop["qtype"] = std::string(dns::rr_type_name(h.qtype));
                hop["responder"] = h.responder;
                hop["latency_us"] = h.latency_us;
                hop["rcode"] = dns::rcode_name(h.rcode);
                hop["note"] = h.note;
                trace_arr.push_back(hop);
            }
            doc["trace"] = trace_arr;
        }

        out << doc.dump(2) << '\n';
        return res;
    }

    if (options.trace && !result.trace.empty()) {
        out << "Trace:\n";
        for (std::size_t i = 0; i < result.trace.size(); ++i) {
            const auto& hop = result.trace[i];
            out << "  " << (i + 1) << ". " << resolver::step_name(hop.step) << " (" << hop.responder << "): "
                << dns::rcode_name(hop.rcode);
            if (hop.latency_us > 0) out << " (" << hop.latency_us << " us)";
            if (!hop.note.empty()) out << " - " << hop.note;
            out << '\n';
        }
        out << '\n';
    }

    out << "Resolved via " << resolver::step_name(result.resolved_step)
        << " (" << result.total_latency_us << " us)"
        << " [" << dns::rcode_name(static_cast<dns::Rcode>(result.response.rcode())) << "]:\n";

    if (result.response.answers.empty()) {
        out << "  (no answers)\n";
    } else {
        for (const auto& a : result.response.answers) {
            out << "  " << a.name.to_string() << ".  " << a.ttl << "  IN  "
                << dns::rr_type_name(a.type) << "  " << format_rdata(a.type, a.rdata) << '\n';
        }
    }

    return res;
}

Result<std::vector<StoredPeer>> peers(CoreApi& api, const PeersOptions& options,
                                      std::ostream& out) {
    auto peers_res = api.peers();
    if (!peers_res) return peers_res.error();

    auto manual_res = api.manual_peers();
    std::vector<std::string> manuals = manual_res.value_or(std::vector<std::string>{});

    if (options.json) {
        json j = json::array();
        for (const auto& p : peers_res.value()) {
            json item;
            item["node_id"] = to_hex(p.node_id);
            item["short_id"] = short_id(p.node_id);
            item["name"] = p.name;
            item["address"] = p.last_addr;
            item["epoch"] = p.epoch;
            item["seq"] = p.seq;
            item["last_seen_epoch_s"] = p.last_seen;
            item["manual"] = false;
            j.push_back(std::move(item));
        }
        for (const auto& m : manuals) {
            bool already = false;
            for (const auto& p : peers_res.value()) {
                if (p.last_addr == m) already = true;
            }
            if (!already) {
                json item;
                item["node_id"] = "";
                item["short_id"] = "";
                item["name"] = "";
                item["address"] = m;
                item["epoch"] = 0;
                item["seq"] = 0;
                item["last_seen_epoch_s"] = 0;
                item["manual"] = true;
                j.push_back(std::move(item));
            }
        }
        out << j.dump(2) << '\n';
        return peers_res;
    }

    if (peers_res.value().empty() && manuals.empty()) {
        out << "no peers\n";
        return peers_res;
    }

    out << "NODE ID    NAME            ADDRESS              EPOCH  SEQ   TYPE\n";
    for (const auto& p : peers_res.value()) {
        std::string sid = short_id(p.node_id);
        out << sid;
        if (sid.size() < 11) out << std::string(11 - sid.size(), ' ');

        std::string name = p.name.empty() ? "(unknown)" : p.name;
        out << name;
        if (name.size() < 16) out << std::string(16 - name.size(), ' ');

        std::string addr = p.last_addr.empty() ? "(none)" : p.last_addr;
        out << addr;
        if (addr.size() < 21) out << std::string(21 - addr.size(), ' ');

        std::string ep_str = std::to_string(p.epoch);
        out << ep_str;
        if (ep_str.size() < 7) out << std::string(7 - ep_str.size(), ' ');

        std::string seq_str = std::to_string(p.seq);
        out << seq_str;
        if (seq_str.size() < 6) out << std::string(6 - seq_str.size(), ' ');

        out << "discovered\n";
    }

    for (const auto& m : manuals) {
        bool already = false;
        for (const auto& p : peers_res.value()) {
            if (p.last_addr == m) already = true;
        }
        if (!already) {
            out << "-          (manual)        ";
            out << m;
            if (m.size() < 21) out << std::string(21 - m.size(), ' ');
            out << "-      -     manual\n";
        }
    }

    return peers_res;
}

Result<void> peer_add(CoreApi& api, const PeerAddOptions& options, std::ostream& out) {
    if (options.endpoint.empty()) return Error{835, "invalid peer endpoint"};
    auto status = api.add_manual_peer(options.endpoint);
    if (!status) return status;

    if (options.json) {
        json j;
        j["added"] = options.endpoint;
        out << j.dump(2) << '\n';
    } else {
        out << "added peer " << options.endpoint << '\n';
    }
    return {};
}

Result<void> peer_rm(CoreApi& api, const PeerRmOptions& options, std::ostream& out) {
    if (options.endpoint.empty()) return Error{835, "invalid peer endpoint"};
    auto status = api.remove_manual_peer(options.endpoint);
    if (!status) return status;

    if (options.json) {
        json j;
        j["removed"] = options.endpoint;
        out << j.dump(2) << '\n';
    } else {
        out << "removed peer " << options.endpoint << '\n';
    }
    return {};
}

Result<void> peer_forget(CoreApi& api, const PeerForgetOptions& options, std::ostream& out) {
    if (options.node.empty()) return Error{836, "node id or prefix required"};
    auto peers = api.peers();
    if (!peers) return peers.error();

    std::optional<NodeId> found;
    for (const auto& p : peers.value()) {
        std::string hx = to_hex(p.node_id);
        std::string sh = short_id(p.node_id);
        if (hx == options.node || sh == options.node || hx.starts_with(options.node)) {
            found = p.node_id;
            break;
        }
    }

    if (!found) {
        return Error{840, "peer not found: " + options.node};
    }

    auto status = api.delete_peer(found.value());
    if (!status) return status.error();

    if (options.json) {
        json j;
        j["forgot"] = to_hex(found.value());
        out << j.dump(2) << '\n';
    } else {
        out << "forgot peer " << to_hex(found.value()) << '\n';
    }
    return {};
}

Result<std::vector<resolver::ConflictRecord>> conflicts(CoreApi& api, const ConflictsOptions& options,
                                                        std::ostream& out) {
    auto records = api.conflicts();
    if (options.json) {
        json j = json::array();
        for (const auto& cr : records) {
            json item;
            item["name"] = cr.name.to_string();
            item["type"] = std::string(dns::rr_type_name(cr.type));
            item["local_divergence"] = cr.local_divergence;
            json groups = json::array();
            for (const auto& g : cr.groups) {
                json grp;
                grp["type"] = std::string(dns::rr_type_name(g.type));
                grp["ttl"] = g.ttl;
                grp["vote_count"] = g.vote_count;
                grp["oldest_claim_created"] = g.oldest_claim_created;
                grp["lowest_node_id"] = to_hex(g.lowest_node_id);
                json recs = json::array();
                for (const auto& d : g.canonical_data) {
                    recs.push_back(format_rdata(g.type, d));
                }
                grp["records"] = recs;
                json cands = json::array();
                for (const auto& c : g.candidates) {
                    json cand;
                    cand["node_id"] = to_hex(c.node_id);
                    cand["online"] = c.online;
                    cand["rtt_ms"] = c.rtt_ms;
                    cand["claim_created_at"] = c.claim_created_at;
                    cands.push_back(cand);
                }
                grp["candidates"] = cands;
                groups.push_back(grp);
            }
            item["groups"] = groups;
            j.push_back(item);
        }
        out << j.dump(2) << '\n';
        return records;
    }

    if (records.empty()) {
        out << "no active conflicts\n";
        return records;
    }

    out << "Active conflicts (" << records.size() << "):\n";
    for (const auto& cr : records) {
        out << "  Domain: " << cr.name.to_string() << " (" << dns::rr_type_name(cr.type) << ")";
        if (cr.local_divergence) {
            out << " [Local divergence]";
        } else {
            out << " [" << cr.groups.size() << " competing groups]";
        }
        out << '\n';
        for (std::size_t i = 0; i < cr.groups.size(); ++i) {
            const auto& g = cr.groups[i];
            out << "    Group " << (i + 1) << " (" << g.vote_count << " vote" << (g.vote_count > 1 ? "s" : "")
                << ", oldest: " << g.oldest_claim_created << ", lowest node: " << short_id(g.lowest_node_id) << "):\n";
            for (const auto& d : g.canonical_data) {
                out << "      - " << format_rdata(g.type, d) << " (TTL " << g.ttl << ")\n";
            }
            out << "      Candidates:\n";
            for (const auto& c : g.candidates) {
                out << "        * " << short_id(c.node_id) << " (" << (c.online ? "online" : "offline")
                    << ", RTT " << c.rtt_ms << "ms)\n";
            }
        }
    }

    return records;
}

Result<DomainInfo> info(CoreApi& api, const InfoOptions& options, std::ostream& out) {
    auto info_res = api.domain_info(options.name, options.type);
    if (!info_res) return info_res.error();

    const auto& di = info_res.value();
    if (options.json) {
        json j;
        j["name"] = di.name.to_string();
        j["type"] = std::string(dns::rr_type_name(di.type));
        j["has_local"] = di.has_local;
        j["local_records"] = di.local_records;
        j["has_conflict"] = di.has_conflict;
        j["local_divergence"] = di.local_divergence;
        json groups = json::array();
        for (std::size_t i = 0; i < di.ranking.groups.size(); ++i) {
            const auto& g = di.ranking.groups[i];
            json grp;
            grp["rank"] = i + 1;
            grp["type"] = std::string(dns::rr_type_name(g.type));
            grp["ttl"] = g.ttl;
            grp["vote_count"] = g.vote_count;
            grp["oldest_claim_created"] = g.oldest_claim_created;
            grp["lowest_node_id"] = to_hex(g.lowest_node_id);
            json recs = json::array();
            for (const auto& d : g.canonical_data) {
                recs.push_back(format_rdata(g.type, d));
            }
            grp["records"] = recs;
            json cands = json::array();
            for (const auto& c : g.candidates) {
                json cand;
                cand["node_id"] = to_hex(c.node_id);
                cand["online"] = c.online;
                cand["rtt_ms"] = c.rtt_ms;
                cand["claim_created_at"] = c.claim_created_at;
                cands.push_back(cand);
            }
            grp["candidates"] = cands;
            groups.push_back(grp);
        }
        j["groups"] = groups;
        out << j.dump(2) << '\n';
        return info_res;
    }

    out << "Domain: " << di.name.to_string() << " (" << dns::rr_type_name(di.type) << ")\n";
    out << "Local Zone:\n";
    if (di.has_local && !di.local_records.empty()) {
        for (const auto& rec : di.local_records) {
            out << "  - " << rec << '\n';
        }
    } else {
        out << "  (none)\n";
    }

    out << "Peer Claims:\n";
    if (di.ranking.groups.empty()) {
        out << "  (none)\n";
    } else {
        for (std::size_t i = 0; i < di.ranking.groups.size(); ++i) {
            const auto& g = di.ranking.groups[i];
            out << "  Group " << (i + 1) << " (" << g.vote_count << " vote" << (g.vote_count > 1 ? "s" : "")
                << ", oldest: " << g.oldest_claim_created << ", lowest node: " << short_id(g.lowest_node_id) << "):\n";
            for (const auto& d : g.canonical_data) {
                out << "    - " << format_rdata(g.type, d) << " (TTL " << g.ttl << ")\n";
            }
            out << "    Candidates:\n";
            for (const auto& c : g.candidates) {
                out << "      * " << short_id(c.node_id) << " (" << (c.online ? "online" : "offline")
                    << ", RTT " << c.rtt_ms << "ms, created: " << c.claim_created_at << ")\n";
            }
        }
    }

    out << "Conflict: ";
    if (di.local_divergence) {
        out << "Local divergence (local record differs from peer claims)\n";
    } else if (di.has_conflict) {
        out << "Multiple competing peer groups (" << di.ranking.groups.size() << ")\n";
    } else {
        out << "None\n";
    }

    return info_res;
}

}  // namespace ldns::cli
