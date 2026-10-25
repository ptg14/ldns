#include "core/runtime/runtime.hpp"

#include <algorithm>
#include <csignal>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "core/domain/self_address.hpp"
#include "core/net/interfaces.hpp"
#include "core/util/logger.hpp"

namespace ldns {
namespace {

constexpr std::uint16_t kDnsPort = 53;

#if defined(_WIN32)
// The handler cannot capture, so it reaches the running Runtime through a
// file-scope pointer. It must only do async-signal-safe work; the actual
// shutdown happens on the io_context thread via post().
Runtime* g_console_runtime = nullptr;

BOOL WINAPI console_ctrl_handler(DWORD event) {
    switch (event) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            if (g_console_runtime != nullptr) g_console_runtime->post_stop();
            return TRUE;
        default:
            return FALSE;
    }
}
#endif

bool is_usable_lan_address(const net::InterfaceAddress& address) {
    if (!address.is_v4()) return false;
    // 127.0.0.0/8 and 169.254.0.0/16 are never answers for a LAN `self` host.
    if (address.bytes[0] == 127) return false;
    return !(address.bytes[0] == 169 && address.bytes[1] == 254);
}

// Adds the default DNS port unless the entry already carries one. Accepts
// "1.2.3.4", "1.2.3.4:5300", "::1" and "[::1]:5300" (SPEC §11 server.listen).
std::string endpoint_for(const std::string& address) {
    if (!address.empty() && address.front() == '[') {
        // "[::1]" has no port yet; "[::1]:5300" already does.
        if (address.find("]:") == std::string::npos) {
            return address + ":" + std::to_string(kDnsPort);
        }
        return address;
    }
    const auto colons = static_cast<std::size_t>(std::count(address.begin(), address.end(), ':'));
    if (colons == 1) {
        // IPv4 with an explicit port.
        return address;
    }
    if (colons > 1) {
        // Bare IPv6 literal.
        return "[" + address + "]:" + std::to_string(kDnsPort);
    }
    return address + ":" + std::to_string(kDnsPort);
}

}  // namespace

Runtime::Runtime(RuntimeOptions options, std::unique_ptr<CoreApi> api)
    : options_(std::move(options)),
      api_(std::move(api)),
      work_guard_(asio::make_work_guard(io_)) {}

Runtime::~Runtime() {
#if defined(_WIN32)
    if (console_handler_installed_) {
        ::SetConsoleCtrlHandler(console_ctrl_handler, FALSE);
        console_handler_installed_ = false;
    }
    if (g_console_runtime == this) g_console_runtime = nullptr;
#endif
    stop();
    work_guard_.reset();
}

Result<std::unique_ptr<Runtime>> Runtime::create(RuntimeOptions options) {
    auto api = CoreApi::create(std::move(options.core));
    if (!api) return api.error();
    return std::unique_ptr<Runtime>(new Runtime(std::move(options), std::move(api.value())));
}

Result<std::vector<std::string>> Runtime::resolve_listen(
    const std::vector<std::string>& configured) const {
    // "auto": loopback plus every LAN address the node currently has, so a
    // phone on the LAN can use this machine as its DNS server (AC-4.4). We do
    // not bind the wildcard address: that would also expose the server on
    // interfaces the user did not choose (SPEC D14).
    const bool auto_mode =
        configured.empty() || std::find(configured.begin(), configured.end(), "auto") !=
                                  configured.end();
    if (!auto_mode) {
        std::vector<std::string> out;
        out.reserve(configured.size());
        for (const auto& entry : configured) out.push_back(endpoint_for(entry));
        return out;
    }

    std::vector<std::string> out;
    out.push_back(endpoint_for("127.0.0.1"));

    auto interfaces = net::list_interfaces();
    if (!interfaces) return interfaces.error();

    // Prefer the interface that carries the default route so the most useful
    // address is bound first; the order also decides nothing else.
    std::vector<const net::InterfaceInfo*> ordered;
    ordered.reserve(interfaces.value().size());
    for (const auto& info : interfaces.value()) ordered.push_back(&info);
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const net::InterfaceInfo* a, const net::InterfaceInfo* b) {
                         if (a->default_route != b->default_route) return a->default_route;
                         if (a->virtual_adapter != b->virtual_adapter) return !a->virtual_adapter;
                         return a->name < b->name;
                     });

    for (const auto* info : ordered) {
        if (!info->up || info->loopback || info->virtual_adapter) continue;
        for (const auto& address : info->addresses) {
            if (!is_usable_lan_address(address)) continue;
            const std::string endpoint = endpoint_for(address.to_string());
            if (std::find(out.begin(), out.end(), endpoint) == out.end()) out.push_back(endpoint);
        }
    }
    return out;
}

Result<void> Runtime::start() {
    if (started_) return Error{320, "runtime already started"};

    auto listen = resolve_listen(options_.listen);
    if (!listen) return listen.error();

    ServerConfig config;
    config.listen = listen.value();
    config.rate_limit_qps = options_.rate_limit_qps;

    handler_ = std::make_unique<QueryHandler>(api_->zone(), HandlerConfig{}, api_->pipeline_shared());
    server_ = std::make_unique<DnsServer>(io_, *handler_, std::move(config));

    if (auto status = server_->start(); !status) {
        server_.reset();
        handler_.reset();
        return Error{321, status.error().message};
    }
    bound_ = server_->bound_addresses();

    // Prevent loop: exclude bound endpoints from upstream queries
    auto up_cfg = api_->upstream_client().config();
    up_cfg.excluded_endpoints = bound_;
    api_->upstream_client().set_config(std::move(up_cfg));

    started_ = true;
    return {};
}

void Runtime::stop() {
    if (server_) {
        server_->stop();
    }
    started_ = false;
    // Release the work guard so run() can return after the current handlers.
    work_guard_.reset();
    io_.stop();
}

void Runtime::run() { io_.run(); }

void Runtime::install_signal_handlers() {
    if (signals_) return;

#if defined(_WIN32)
    // Asio's signal_set only wraps ::signal(). On Windows the CRT raises
    // SIGINT for a console Ctrl+C only when a SetConsoleCtrlHandler hook is
    // installed, so the standard handler is what actually receives the event.
    // It runs on a separate thread: post the stop to the io_context rather
    // than touching it directly.
    g_console_runtime = this;
    console_handler_installed_ = ::SetConsoleCtrlHandler(console_ctrl_handler, TRUE) != 0;
    if (!console_handler_installed_) {
        logger()->warn("could not install the console control handler; use Task Manager "
                       "to stop the daemon");
    }
#else
    signals_ = std::make_unique<asio::signal_set>(io_, SIGINT, SIGTERM);
    signals_->async_wait([this](const asio::error_code& ec, int) {
        if (ec) return;
        logger()->info("received shutdown signal");
        request_stop();
    });
#endif
}

void Runtime::request_stop() {
    stop_requested_ = true;
    work_guard_.reset();
    // Defer to the io_context thread: callers may be signal handlers or
    // other threads, and io_context::stop() is not safe against a concurrent
    // run() on all platforms.
    asio::post(io_, [this] { io_.stop(); });
}

void Runtime::post_stop() {
    try {
        asio::post(io_, [this] {
            stop_requested_ = true;
            work_guard_.reset();
            io_.stop();
        });
    } catch (...) {
        // The io_context is gone or shutting down; nothing left to stop.
    }
}

const std::vector<std::string>& Runtime::bound_addresses() const { return bound_; }

}  // namespace ldns
