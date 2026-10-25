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

}  // namespace ldns::cli
