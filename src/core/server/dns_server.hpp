#pragma once

// DNS transport (SPEC §8, PLAN M1): UDP and TCP listeners that share one
// QueryHandler.
//
// - Binds explicit addresses only (SPEC D14); never 0.0.0.0.
// - ACL and token-bucket rate limiting run before any parsing.
// - UDP responses larger than the client's payload are truncated (TC=1);
//   TCP uses the 2-byte length prefix and is capped at 64 KiB.
// - All I/O runs on the caller-provided Asio io_context.

#include <asio.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/server/acl.hpp"
#include "core/server/query_handler.hpp"
#include "core/util/result.hpp"

namespace ldns {

struct ServerConfig {
    // "127.0.0.1:53", "192.168.1.10:53", "[::1]:53". Multiple entries bind
    // multiple sockets (one UDP + one TCP per address).
    std::vector<std::string> listen{"127.0.0.1:53"};

    std::uint32_t rate_limit_qps{200};
    std::uint32_t rate_limit_burst{0};  // 0 = same as qps
    bool allow_private_sources{true};

    std::size_t max_tcp_connections{128};
    // How long a TCP connection may sit idle between queries.
    std::uint32_t tcp_idle_seconds{10};

    // For tests and unprivileged runs.
    std::vector<std::string> extra_allow;
    std::vector<std::string> deny;
};

// Tracks live sockets so a partial bind failure can be reported and rolled
// back cleanly (SPEC §13.1).
class DnsServer {
public:
    DnsServer(asio::io_context& io, const QueryHandler& handler, ServerConfig config);
    ~DnsServer();

    DnsServer(const DnsServer&) = delete;
    DnsServer& operator=(const DnsServer&) = delete;

    // Binds every configured address. On failure returns the first error with
    // the address that failed; already-bound sockets are closed.
    Result<void> start();

    void stop();

    std::vector<std::string> bound_addresses() const;

    Acl& acl() noexcept { return acl_; }
    RateLimiter& rate_limiter() noexcept { return limiter_; }

private:
    class UdpListener;
    class TcpListener;

    asio::io_context& io_;
    const QueryHandler& handler_;
    ServerConfig config_;
    // Listeners hold a non-const reference: the server is the single owner of
    // these and all listener callbacks run on the same io_context thread.
    Acl acl_;
    RateLimiter limiter_;

    std::vector<std::shared_ptr<UdpListener>> udp_;
    std::vector<std::shared_ptr<TcpListener>> tcp_;
    bool started_{false};
};

}  // namespace ldns
