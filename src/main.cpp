// Ldns entry point (SPEC §10.1).
//
//   ldns                       -> TUI (M7; until then: prints help)
//   ldns daemon [--foreground] -> headless DNS server (the service entry)
//   ldns <command> [args...]   -> one-shot CLI, then exit
//   ldns --version | --help
//
// Exit codes are part of the contract (SPEC §10.1):
//   0 success · 1 general error · 2 bad arguments · 3 daemon unreachable
//   4 missing privilege · 5 conflict/already exists
//
// Commands are thin wrappers over CoreApi (src/core/cli). The TUI reuses the
// same CoreApi calls, so behaviour never diverges between the two.

#include <CLI/CLI.hpp>

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "core/cli/commands.hpp"
#include "core/config/config.hpp"
#include "core/config/paths.hpp"
#include "core/core_api.hpp"
#include "core/runtime/runtime.hpp"
#include "core/util/logger.hpp"
#include "core/version.hpp"
#include "ipc/ipc_server.hpp"
#include "ipc/ipc_client.hpp"
#include "os/doctor.hpp"
#include "os/system_dns_manager.hpp"
#include "os/service.hpp"
#include "os/firewall.hpp"

namespace {

using namespace ldns;


struct GlobalOptions {
    std::string config_file;
    std::string data_file;
    std::string log_level{"info"};
};

// Option values collected by CLI11 for every subcommand. Unused fields keep
// their defaults, so one struct serves all commands.
struct CommandOptions {
    std::string name;
    std::vector<std::string> targets;
    std::string type{"A"};
    std::uint32_t ttl{0};
    bool force{false};
    bool json{false};
    std::string file;
    std::string out;
    bool csv{false};
    bool foreground{false};
    bool trace{false};
};

// Resolves paths and loads config.toml + LDNS_* environment (SPEC §11
// precedence: CLI flags > environment > config.toml > defaults).
Result<config::Config> load_config(const GlobalOptions& options, config::Paths& paths) {
    paths = config::default_paths(config::InstallKind::User);
    if (!options.config_file.empty()) paths.config_file = options.config_file;
    if (!options.data_file.empty()) paths.data_file = options.data_file;

    config::Config config;
    if (auto status = config::Config::load_file(paths.config_file, config); !status) {
        return status.error();
    }
    config.apply_environment();
    if (auto status = config.validate(); !status) return status.error();
    return config;
}

CoreApi::Options api_options(config::Config config, const config::Paths& paths) {
    CoreApi::Options out;
    out.config = std::move(config);
    out.paths = paths;
    return out;
}

int fail(const Error& error) {
    std::cerr << "error: " << error.message << '\n';
    return cli::exit_code_for(error);
}

int run_daemon(const GlobalOptions& global, bool /*foreground*/) {
    config::Paths paths;
    auto config = load_config(global, paths);
    if (!config) return fail(config.error());

    RuntimeOptions options;
    options.core = api_options(std::move(config.value()), paths);
    options.listen = options.core.config.server.listen;
    options.rate_limit_qps = options.core.config.server.rate_limit_qps;

    auto runtime = Runtime::create(std::move(options));
    if (!runtime) return fail(runtime.error());

    if (auto status = runtime.value()->start(); !status) {
        std::cerr << "error: " << status.error().message << '\n';
        std::cerr << "hint: port 53 may be held by another resolver. On Linux "
                     "systemd-resolved keeps 127.0.0.53, which does not conflict; "
                     "set [server].listen explicitly to choose addresses\n";
        return cli::kExitPrivilege;
    }

    const auto& bound = runtime.value()->bound_addresses();
    std::cout << "ldns " << version_string() << " serving " << bound.size()
              << " address(es):";
    for (const auto& address : bound) std::cout << ' ' << address;
    std::cout << '\n';
    std::cout.flush();

    // Start IPC JSON-RPC 2.0 server (SPEC §10.1, §12, PLAN M8)
    ipc::IpcServer ipc_server(runtime.value()->api());
    ipc_server.set_shutdown_handler([&]() {
        runtime.value()->request_stop();
    });
    (void)ipc_server.start();

    // SIGINT/SIGTERM (and the Windows console events) request a stop on the
    // io_context thread, so run() returns and the destructor closes the
    // sockets. See Runtime::install_signal_handlers.
    runtime.value()->install_signal_handlers();

    runtime.value()->run();
    ipc_server.stop();
    logger()->info("shutdown complete");
    return cli::kExitOk;
}

// Opens CoreApi for a one-shot command.
Result<std::unique_ptr<CoreApi>> open_api(const GlobalOptions& global) {
    config::Paths paths;
    auto config = load_config(global, paths);
    if (!config) return config.error();
    return CoreApi::create(api_options(std::move(config.value()), paths));
}

int run_command(const std::string& command, const GlobalOptions& global,
                const CommandOptions& options, const std::string& list_type,
                const std::string& peer_subaction = "") {
    if (command == "network") {
        config::Paths paths;
        (void)load_config(global, paths);
        cli::NetworkKeyOptions opt;
        opt.action = peer_subaction;
        opt.path = options.file;
        opt.json = options.json;
        auto result = cli::network_key_cmd(paths, opt);
        if (!result) return fail(result.error());
        return cli::kExitOk;
    }

    auto api = open_api(global);
    if (!api) return fail(api.error());
    CoreApi& core = *api.value();

    if (command == "add" || command == "set") {
        cli::AddOptions request;
        request.name = options.name;
        request.targets = options.targets;
        request.type = options.type;
        request.ttl = options.ttl;
        request.force = options.force;
        request.json = options.json;
        auto result = command == "add" ? cli::add(core, request) : cli::set(core, request);
        if (!result) return fail(result.error());
        return cli::kExitOk;
    }

    if (command == "rm") {
        auto result = cli::remove(core, options.name, options.type);
        if (!result) return fail(result.error());
        if (options.json) {
            std::cout << "{\"removed\":\"" << options.name << "\"}\n";
        } else {
            std::cout << "removed " << options.name << '\n';
        }
        return cli::kExitOk;
    }

    if (command == "ls") {
        cli::ListOptions request;
        if (!list_type.empty()) request.type = list_type;
        request.json = options.json;
        auto result = cli::list(core, request);
        if (!result) return fail(result.error());
        return cli::kExitOk;
    }

    if (command == "status") {
        auto result = cli::status(core, options.json);
        if (!result) return fail(result.error());
        return cli::kExitOk;
    }

    if (command == "import") {
        cli::ImportOptions request;
        request.path = options.file;
        request.force = options.force;
        request.json = options.json;
        auto result = cli::import_file(core, request);
        if (!result) return fail(result.error());
        return cli::kExitOk;
    }

    if (command == "export") {
        cli::ExportOptions request;
        if (!options.out.empty()) request.path = options.out;
        request.csv = options.csv;
        request.json = options.json;
        auto result = cli::export_zone(core, request);
        if (!result) return fail(result.error());
        return cli::kExitOk;
    }

    if (command == "resolve") {
        cli::ResolveOptions opt;
        opt.name = options.name;
        opt.type = options.type;
        opt.trace = options.trace;
        opt.json = options.json;
        auto result = cli::resolve(core, opt);
        if (!result) return fail(result.error());
        return cli::kExitOk;
    }

    if (command == "peers") {
        cli::PeersOptions opt;
        opt.json = options.json;
        auto result = cli::peers(core, opt);
        if (!result) return fail(result.error());
        return cli::kExitOk;
    }

    if (command == "conflicts") {
        cli::ConflictsOptions opt;
        opt.json = options.json;
        auto result = cli::conflicts(core, opt);
        if (!result) return fail(result.error());
        return cli::kExitOk;
    }

    if (command == "info") {
        cli::InfoOptions opt;
        opt.name = options.name;
        opt.type = options.type;
        opt.json = options.json;
        auto result = cli::info(core, opt);
        if (!result) return fail(result.error());
        return cli::kExitOk;
    }

    if (command == "peer") {
        if (peer_subaction == "add") {
            cli::PeerAddOptions opt;
            opt.endpoint = options.name;
            opt.json = options.json;
            auto result = cli::peer_add(core, opt);
            if (!result) return fail(result.error());
            return cli::kExitOk;
        }
        if (peer_subaction == "rm") {
            cli::PeerRmOptions opt;
            opt.endpoint = options.name;
            opt.json = options.json;
            auto result = cli::peer_rm(core, opt);
            if (!result) return fail(result.error());
            return cli::kExitOk;
        }
        if (peer_subaction == "forget") {
            cli::PeerForgetOptions opt;
            opt.node = options.name;
            opt.json = options.json;
            auto result = cli::peer_forget(core, opt);
            if (!result) return fail(result.error());
            return cli::kExitOk;
        }
    }
    if (command == "doctor") {
        cli::DoctorOptions opt;
        opt.json = options.json;
        auto result = cli::doctor(core, opt);
        if (!result) return fail(result.error());
        return cli::kExitOk;
    }

    if (command == "system") {
        cli::SystemOptions opt;
        opt.action = peer_subaction;
        opt.json = options.json;
        auto result = cli::system_cmd(core, opt);
        if (!result) return fail(result.error());
        return cli::kExitOk;
    }

    if (command == "service") {
        cli::ServiceOptions opt;
        opt.action = peer_subaction;
        opt.binary_path = options.file;
        opt.json = options.json;
        auto result = cli::service_cmd(opt);
        if (!result) return fail(result.error());
        return cli::kExitOk;
    }

    if (command == "firewall") {
        cli::FirewallOptions opt;
        opt.action = peer_subaction;
        opt.json = options.json;
        auto result = cli::firewall_cmd(opt);
        if (!result) return fail(result.error());
        return cli::kExitOk;
    }

    std::cerr << "error: unknown command\n";
    return cli::kExitUsage;
}

}  // namespace

int main(int argc, char** argv) {
    CLI::App app{"Ldns — peer-to-peer local DNS for LAN", "ldns"};
    app.require_subcommand(0, 1);
    app.set_version_flag(
        "--version", ldns::version_string() + " (protocol " +
                         std::to_string(ldns::kProtocolMajor) + "." +
                         std::to_string(ldns::kProtocolMinor) + ")");

    GlobalOptions global;
    app.add_option("--config", global.config_file,
                   "config.toml path (default: per-OS, SPEC §12)");
    app.add_option("--data", global.data_file, "database path (default: per-OS, SPEC §12)");
    app.add_option("--log-level", global.log_level, "trace|debug|info|warn|error")
        ->check(CLI::IsMember({"trace", "debug", "info", "warn", "warning", "error", "off"}))
        ->default_val("info");

    CommandOptions options;
    // `ls --type` filters; it must default to "no filter", unlike add/set/rm
    // where the record type defaults to A. Sharing one variable would leak the
    // "A" default into the listing.
    std::string list_type;

    auto* add = app.add_subcommand("add", "create a domain (target defaults to self)");
    add->add_option("name", options.name, "domain name")->required();
    add->add_option("targets", options.targets, "targets (default: self)");
    add->add_option("--type", options.type, "A|AAAA|CNAME|TXT")->default_val("A");
    add->add_option("--ttl", options.ttl, "TTL in seconds");
    add->add_flag("--force", options.force, "skip the public-TLD confirmation (SPEC §9.1)");
    add->add_flag("--json", options.json, "machine-readable output");

    auto* set = app.add_subcommand("set", "change the targets of an existing domain");
    set->add_option("name", options.name, "domain name")->required();
    set->add_option("targets", options.targets, "new targets")->required();
    set->add_option("--type", options.type, "A|AAAA|CNAME|TXT")->default_val("A");
    set->add_option("--ttl", options.ttl, "TTL in seconds");
    set->add_flag("--force", options.force, "skip the public-TLD confirmation");
    set->add_flag("--json", options.json, "machine-readable output");

    auto* rm = app.add_subcommand("rm", "delete a domain");
    rm->add_option("name", options.name, "domain name")->required();
    rm->add_option("--type", options.type, "A|AAAA|CNAME|TXT")->default_val("A");
    rm->add_flag("--json", options.json, "machine-readable output");

    auto* ls = app.add_subcommand("ls", "list this node's domains");
    ls->add_option("--type", list_type, "filter by record type (default: all)");
    ls->add_flag("--json", options.json, "machine-readable output");

    auto* status = app.add_subcommand("status", "show node, zone and warnings");
    status->add_flag("--json", options.json, "machine-readable output");

    auto* import = app.add_subcommand("import", "bulk import a .toml or .csv file");
    import->add_option("file", options.file, "file.toml or file.csv")->required();
    import->add_flag("--force", options.force, "skip the public-TLD confirmation");
    import->add_flag("--json", options.json, "machine-readable output");

    auto* export_cmd = app.add_subcommand("export", "export this node's zone");
    export_cmd->add_option("--out", options.out, "write to a file instead of stdout");
    export_cmd->add_flag("--csv", options.csv, "CSV instead of TOML");
    export_cmd->add_flag("--json", options.json, "machine-readable output");

    auto* resolve_cmd = app.add_subcommand("resolve", "resolve a DNS name through the pipeline");
    resolve_cmd->add_option("name", options.name, "domain name to resolve")->required();
    resolve_cmd->add_option("type", options.type, "record type (default: A)")->default_val("A");
    resolve_cmd->add_flag("--trace", options.trace, "print resolution trace hops");
    resolve_cmd->add_flag("--json", options.json, "machine-readable output");

    auto* peers_cmd = app.add_subcommand("peers", "list discovered and manual peers");
    peers_cmd->add_flag("--json", options.json, "machine-readable output");

    auto* peer_cmd = app.add_subcommand("peer", "manage peers (add, rm, forget)");
    peer_cmd->require_subcommand(1, 1);

    auto* peer_add_cmd = peer_cmd->add_subcommand("add", "add a manual peer (ip[:port])");
    peer_add_cmd->add_option("endpoint", options.name, "peer endpoint (ip[:port])")->required();
    peer_add_cmd->add_flag("--json", options.json, "machine-readable output");

    auto* peer_rm_cmd = peer_cmd->add_subcommand("rm", "remove a manual peer");
    peer_rm_cmd->add_option("endpoint", options.name, "peer endpoint (ip[:port])")->required();
    peer_rm_cmd->add_flag("--json", options.json, "machine-readable output");

    auto* peer_forget_cmd = peer_cmd->add_subcommand("forget", "remove a peer from stored registry");
    peer_forget_cmd->add_option("node", options.name, "peer node ID or prefix")->required();
    peer_forget_cmd->add_flag("--json", options.json, "machine-readable output");

    auto* conflicts_cmd = app.add_subcommand("conflicts", "show active domain conflicts between peers");
    conflicts_cmd->add_flag("--json", options.json, "machine-readable output");

    auto* info_cmd = app.add_subcommand("info", "show peer claims, voting groups, and ranking for a domain");
    info_cmd->add_option("name", options.name, "domain name")->required();
    info_cmd->add_option("--type", options.type, "record type (default: A)")->default_val("A");
    info_cmd->add_flag("--json", options.json, "machine-readable output");

    auto* daemon = app.add_subcommand("daemon", "run the DNS server headless with IPC");
    daemon->add_flag("--foreground", options.foreground,
                     "stay in the foreground (default)");
    daemon->add_flag("--service", options.foreground,
                     "run as a system service (systemd / Windows SCM)");

    auto* doctor_cmd = app.add_subcommand("doctor", "diagnose system environment (port 53, OS DNS, firewall, multicast)");
    doctor_cmd->add_flag("--json", options.json, "machine-readable output");

    auto* system_cmd = app.add_subcommand("system", "manage OS DNS configuration (setup, restore, status)");
    system_cmd->require_subcommand(1, 1);
    auto* sys_setup = system_cmd->add_subcommand("setup", "configure OS DNS to 127.0.0.1 (with automatic backup)");
    sys_setup->add_flag("--json", options.json, "machine-readable output");
    auto* sys_restore = system_cmd->add_subcommand("restore", "restore OS DNS to original backup configuration");
    sys_restore->add_flag("--json", options.json, "machine-readable output");
    auto* sys_status = system_cmd->add_subcommand("status", "show current OS DNS routing status");
    sys_status->add_flag("--json", options.json, "machine-readable output");

    auto* service_cmd = app.add_subcommand("service", "manage background OS service (install, uninstall, start, stop, status)");
    service_cmd->require_subcommand(1, 1);
    auto* svc_install = service_cmd->add_subcommand("install", "install Ldns as an OS background service");
    svc_install->add_option("--bin", options.file, "path to binary");
    svc_install->add_flag("--json", options.json, "machine-readable output");
    auto* svc_uninstall = service_cmd->add_subcommand("uninstall", "uninstall Ldns OS service");
    svc_uninstall->add_flag("--json", options.json, "machine-readable output");
    auto* svc_start = service_cmd->add_subcommand("start", "start Ldns OS service");
    svc_start->add_flag("--json", options.json, "machine-readable output");
    auto* svc_stop = service_cmd->add_subcommand("stop", "stop Ldns OS service");
    svc_stop->add_flag("--json", options.json, "machine-readable output");
    auto* svc_status = service_cmd->add_subcommand("status", "check Ldns OS service status");
    svc_status->add_flag("--json", options.json, "machine-readable output");

    auto* firewall_cmd = app.add_subcommand("firewall", "manage LAN firewall rules for Ldns (53, 5390, 5391)");
    firewall_cmd->require_subcommand(1, 1);
    auto* fw_add = firewall_cmd->add_subcommand("add", "add firewall rules for local LAN ports");
    fw_add->add_flag("--json", options.json, "machine-readable output");
    auto* fw_rm = firewall_cmd->add_subcommand("remove", "remove Ldns firewall rules");
    fw_rm->add_flag("--json", options.json, "machine-readable output");
    auto* fw_status = firewall_cmd->add_subcommand("status", "show Ldns firewall rule status");
    fw_status->add_flag("--json", options.json, "machine-readable output");

    auto* network_cmd = app.add_subcommand("network", "manage network configuration and PSK");
    network_cmd->require_subcommand(1, 1);
    auto* net_key = network_cmd->add_subcommand("key", "manage network PSK key (generate, set, clear, status)");
    net_key->require_subcommand(1, 1);
    auto* net_key_gen = net_key->add_subcommand("generate", "generate a new random 256-bit PSK file");
    net_key_gen->add_option("path", options.file, "path to write key file");
    net_key_gen->add_flag("--json", options.json, "machine-readable output");
    auto* net_key_set = net_key->add_subcommand("set", "set network.key_file path in config.toml");
    net_key_set->add_option("path", options.file, "path to existing key file")->required();
    net_key_set->add_flag("--json", options.json, "machine-readable output");
    auto* net_key_clear = net_key->add_subcommand("clear", "clear network.key_file in config.toml");
    net_key_clear->add_flag("--json", options.json, "machine-readable output");
    auto* net_key_status = net_key->add_subcommand("status", "check PSK status");
    net_key_status->add_flag("--json", options.json, "machine-readable output");

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        // --help/--version are not errors; anything else is a usage error.
        const int cli_code = app.exit(e);
        return cli_code == 0 ? cli::kExitOk : cli::kExitUsage;
    }

    const std::string command = app.get_subcommands().empty()
                                    ? std::string()
                                    : app.get_subcommands().front()->get_name();

    if (command.empty()) {
        // The TUI arrives in M7. Until then be explicit instead of pretending
        // to start one.
        std::cout << "ldns " << version_string() << '\n'
                  << "the interactive TUI is not built yet; use a command:\n"
                  << "  ldns add app.lan [10.0.0.5] | set | rm | ls | status\n"
                  << "  ldns resolve google.com [A] [--trace] [--json]\n"
                  << "  ldns import file.toml | export --out file.toml\n"
                  << "  ldns daemon --foreground\n"
                  << "Run `ldns --help` for the full list.\n";
        return cli::kExitOk;
    }

    ldns::init_logger(global.log_level);

    std::string peer_subaction;
    if (command == "peer" && !peer_cmd->get_subcommands().empty()) {
        peer_subaction = peer_cmd->get_subcommands().front()->get_name();
    } else if (command == "system" && !system_cmd->get_subcommands().empty()) {
        peer_subaction = system_cmd->get_subcommands().front()->get_name();
    } else if (command == "service" && !service_cmd->get_subcommands().empty()) {
        peer_subaction = service_cmd->get_subcommands().front()->get_name();
    } else if (command == "firewall" && !firewall_cmd->get_subcommands().empty()) {
        peer_subaction = firewall_cmd->get_subcommands().front()->get_name();
    } else if (command == "network" && !network_cmd->get_subcommands().empty()) {
        auto* sub = network_cmd->get_subcommands().front();
        if (!sub->get_subcommands().empty()) {
            peer_subaction = sub->get_subcommands().front()->get_name();
        }
    }

    if (command == "daemon") return run_daemon(global, options.foreground);
    return run_command(command, global, options, list_type, peer_subaction);
}
