#pragma once

// One-shot CLI commands (SPEC §10.1, §10.3; PLAN M2).
//
// Every command is a thin wrapper over CoreApi: no command touches the Store,
// the zone or the config directly. The TUI (M7) will register the same names
// in its slash menu, so a command's behaviour is defined once, here.
//
// Output contract:
//   - human mode prints lines meant to be read by a person;
//   - --json prints one JSON document on stdout and nothing else, so scripts
//     can parse it (SPEC AC-4.3);
//   - errors go to stderr and set the exit code from SPEC §10.1.
//
// Exit codes (SPEC §10.1): 0 success · 1 general error · 2 bad arguments ·
// 3 daemon unreachable · 4 missing privilege · 5 conflict/already exists.

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/core_api.hpp"
#include "core/resolver/pipeline.hpp"
#include "core/util/result.hpp"

namespace ldns::cli {

enum ExitCode : int {
    kExitOk = 0,
    kExitError = 1,
    kExitUsage = 2,
    kExitNoDaemon = 3,
    kExitPrivilege = 4,
    kExitConflict = 5,
};

// Maps a domain/store error code to the process exit code. Codes are the
// stable ones documented in domain_service.hpp and store.hpp.
int exit_code_for(const Error& error);

// Convenience: `ldns add NAME [TARGET...]`.
struct AddOptions {
    std::string name;
    std::vector<std::string> targets;
    std::string type{"A"};
    std::uint32_t ttl{0};  // 0 = [domains].default_ttl
    bool force{false};
    bool json{false};
};

Result<domain::DomainEntry> add(CoreApi& api, const AddOptions& options);
Result<domain::DomainEntry> set(CoreApi& api, const AddOptions& options);
Result<void> remove(CoreApi& api, std::string_view name, std::string_view type);

struct ListOptions {
    std::optional<std::string> type;
    bool json{false};
};
Result<std::vector<domain::DomainEntry>> list(CoreApi& api, const ListOptions& options);

Result<std::size_t> status(CoreApi& api, bool json);

struct ImportOptions {
    std::string path;
    bool force{false};
    bool json{false};
};
Result<domain::ImportReport> import_file(CoreApi& api, const ImportOptions& options);

struct ExportOptions {
    std::optional<std::string> path;  // nullopt = stdout
    bool csv{false};
    bool json{false};
};
Result<void> export_zone(CoreApi& api, const ExportOptions& options);

struct ResolveOptions {
    std::string name;
    std::string type{"A"};
    bool trace{false};
    bool json{false};
};
Result<resolver::PipelineResult> resolve(CoreApi& api, const ResolveOptions& options,
                                         std::ostream& out = std::cout);

struct PeersOptions {
    bool json{false};
};
Result<std::vector<StoredPeer>> peers(CoreApi& api, const PeersOptions& options,
                                      std::ostream& out = std::cout);

struct PeerAddOptions {
    std::string endpoint;
    bool json{false};
};
Result<void> peer_add(CoreApi& api, const PeerAddOptions& options,
                      std::ostream& out = std::cout);

struct PeerRmOptions {
    std::string endpoint;
    bool json{false};
};
Result<void> peer_rm(CoreApi& api, const PeerRmOptions& options,
                     std::ostream& out = std::cout);

struct PeerForgetOptions {
    std::string node;
    bool json{false};
};
Result<void> peer_forget(CoreApi& api, const PeerForgetOptions& options,
                         std::ostream& out = std::cout);

}  // namespace ldns::cli
