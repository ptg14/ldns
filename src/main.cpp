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

int run_daemon(const GlobalOptions& global, bool foreground) {
    if (!foreground) {
        // M8 adds the service/daemon split: the background process is started
        // by the OS service manager, not by a self-detaching fork.
        std::cerr << "error: `ldns daemon` needs --foreground until the service "
                     "integration lands (M8); run: ldns daemon --foreground\n";
        return cli::kExitUsage;
    }

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

    // SIGINT/SIGTERM (and the Windows console events) request a stop on the
    // io_context thread, so run() returns and the destructor closes the
    // sockets. See Runtime::install_signal_handlers.
    runtime.value()->install_signal_handlers();

    runtime.value()->run();
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
                const CommandOptions& options, const std::string& list_type) {
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

    auto* daemon = app.add_subcommand("daemon", "run the DNS server headless");
    daemon->add_flag("--foreground", options.foreground,
                     "stay in the foreground (required until M8)");

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
                  << "  ldns import file.toml | export --out file.toml\n"
                  << "  ldns daemon --foreground\n"
                  << "Run `ldns --help` for the full list.\n";
        return cli::kExitOk;
    }

    ldns::init_logger(global.log_level);

    if (command == "daemon") return run_daemon(global, options.foreground);
    return run_command(command, global, options, list_type);
}
