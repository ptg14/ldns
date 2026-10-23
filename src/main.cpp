#include <CLI/CLI.hpp>

#include <iostream>
#include <string>

#include "core/util/logger.hpp"
#include "core/version.hpp"
#include "ui/tui_app.hpp"

namespace {

// Exit codes are part of the CLI contract (SPEC §10.1):
//   0 success · 1 general error · 2 bad arguments · 3 no daemon
//   4 missing privilege · 5 conflict/already exists
constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;

}  // namespace

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

int main(int argc, char** argv) {
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    CLI::App app{"Ldns — peer-to-peer local DNS for LAN", "ldns"};
    app.set_version_flag("--version", ldns::version_string() + " (protocol " +
                                         std::to_string(ldns::kProtocolMajor) + "." +
                                         std::to_string(ldns::kProtocolMinor) + ")");

    std::string log_level = "info";
    app.add_option("--log-level", log_level, "trace|debug|info|warn|error")
        ->check(CLI::IsMember({"trace", "debug", "info", "warn", "warning", "error", "off"}))
        ->default_val("info");

    bool headless = false;
    app.add_flag("--headless", headless, "Run in headless mode without TUI");

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        // --help/--version are not errors; anything else is a usage error.
        const int cli_code = app.exit(e);
        return cli_code == 0 ? kExitOk : kExitUsage;
    }

    ldns::init_logger(log_level);

    if (headless) {
        ldns::logger()->info("ldns {} - running headless", ldns::version_string());
        return kExitOk;
    }

    // Launch interactive Claude Code style TUI
    ldns::ui::TuiApp tui;
    return tui.run();
}
