#include <CLI/CLI.hpp>

#include <iostream>
#include <string>

#include "core/util/logger.hpp"
#include "core/version.hpp"

namespace {

// Exit codes are part of the CLI contract (SPEC §10.1):
//   0 success · 1 general error · 2 bad arguments · 3 no daemon
//   4 missing privilege · 5 conflict/already exists
constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;

}  // namespace

int main(int argc, char** argv) {
    CLI::App app{"Ldns — peer-to-peer local DNS for LAN", "ldns"};
    app.set_version_flag("--version", ldns::version_string() + " (protocol " +
                                         std::to_string(ldns::kProtocolMajor) + "." +
                                         std::to_string(ldns::kProtocolMinor) + ")");

    std::string log_level = "info";
    app.add_option("--log-level", log_level, "trace|debug|info|warn|error")
        ->check(CLI::IsMember({"trace", "debug", "info", "warn", "warning", "error", "off"}))
        ->default_val("info");

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        // --help/--version are not errors; anything else is a usage error.
        const int cli_code = app.exit(e);
        return cli_code == 0 ? kExitOk : kExitUsage;
    }

    ldns::init_logger(log_level);
    ldns::logger()->info("ldns {} — M0 skeleton", ldns::version_string());
    return kExitOk;
}
