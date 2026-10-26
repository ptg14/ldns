#pragma once

// Runtime: CoreApi + DNS server on one io_context (SPEC §4, PLAN M2/M3).
//
// This is the "embedded core" that both `ldns daemon --foreground` and the
// TUI host. It owns the io_context, the DnsServer and the CoreApi, and turns
// the configured listen list into bound sockets. Keeping it separate from
// main.cpp means the TUI and the daemon start the core in exactly the same
// way.
//
// Lifetime rule (SPEC D14 / §13.1): the DNS server references the LocalZone
// owned by CoreApi, so CoreApi must outlive the server. Runtime enforces that
// by declaration order and by stopping the server before the api is released.

#include <asio.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/config/config.hpp"
#include "core/config/paths.hpp"
#include "core/core_api.hpp"
#include "core/server/dns_server.hpp"
#include "core/net/discovery.hpp"
#include "core/net/peer_manager.hpp"
#include "core/registry/sync_manager.hpp"
#include "core/util/result.hpp"

namespace ldns {

struct RuntimeOptions {
    CoreApi::Options core;
    // Resolved at start(): "auto" expands to the LAN addresses plus loopback,
    // explicit entries are used verbatim. Never a wildcard (SPEC D14).
    std::vector<std::string> listen;
    std::uint32_t rate_limit_qps{200};
};

class Runtime {
public:
    static Result<std::unique_ptr<Runtime>> create(RuntimeOptions options);

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;
    ~Runtime();

    // Binds every DNS socket and starts accepting. Safe to call once.
    Result<void> start();

    void stop();

    // Runs the io_context until stop() is called or the context runs out of
    // work. Returns after all handlers completed.
    void run();

    // Installs async handlers for SIGINT/SIGTERM that call request_stop().
    // Asio delivers them on the io_context thread, so no work happens inside
    // a signal handler. Safe to call once, after start() or before run().
    void install_signal_handlers();

    // Lets a signal handler or the TUI request a clean shutdown.
    void request_stop();

    // Same, but safe to call from a foreign thread (the Windows console
    // handler): the stop is posted to the io_context thread.
    void post_stop();

    // True once a signal handler asked us to stop; the process exits 0 in
    // that case, distinguishing a clean shutdown from a fatal error.
    bool stop_requested() const noexcept { return stop_requested_; }

    CoreApi& api() noexcept { return *api_; }
    asio::io_context& io() noexcept { return io_; }
    const std::vector<std::string>& bound_addresses() const;
    net::PeerManager* peer_manager() noexcept { return peer_manager_.get(); }
    net::DiscoveryService* discovery() noexcept { return discovery_.get(); }
    SyncManager* sync_manager() noexcept { return sync_manager_.get(); }

private:
    Runtime(RuntimeOptions options, std::unique_ptr<CoreApi> api);

    // Expands "auto" into loopback + the node's current LAN addresses.
    Result<std::vector<std::string>> resolve_listen(
        const std::vector<std::string>& configured) const;

    RuntimeOptions options_;
    asio::io_context io_;
    // Declared before server_: the server holds a reference to the zone that
    // CoreApi owns.
    std::unique_ptr<CoreApi> api_;
    std::unique_ptr<QueryHandler> handler_;
    std::unique_ptr<DnsServer> server_;
    std::unique_ptr<asio::signal_set> signals_;
    asio::executor_work_guard<asio::io_context::executor_type> work_guard_;
    bool started_{false};
    bool stop_requested_{false};
#if defined(_WIN32)
    bool console_handler_installed_{false};
#endif
    std::vector<std::string> bound_;
    void start_interface_monitor();

    std::shared_ptr<net::PeerManager> peer_manager_;
    std::shared_ptr<net::DiscoveryService> discovery_;
    std::shared_ptr<SyncManager> sync_manager_;
    std::unique_ptr<asio::steady_timer> interface_monitor_timer_;
};

}  // namespace ldns
