#include "core/server/dns_server.hpp"

#include <array>
#include <chrono>
#include <functional>
#include <utility>

namespace ldns {
namespace {

using asio::ip::tcp;
using asio::ip::udp;

std::int64_t now_ns() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
}

Result<std::uint16_t> parse_port(std::string_view text) {
    if (text.empty() || text.size() > 5) return Error{310, "invalid port"};
    int value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') return Error{310, "invalid port"};
        value = value * 10 + (c - '0');
    }
    if (value > 65535) return Error{310, "port out of range"};
    return static_cast<std::uint16_t>(value);
}

// "1.2.3.4:53", "[::1]:53" or bare "1.2.3.4" (port defaults to 53).
// Never accepts 0.0.0.0 / :: as a bind address (SPEC D14).
Result<std::pair<IpAddress, std::uint16_t>> parse_endpoint(std::string_view text) {
    std::string_view address = text;
    std::uint16_t port = 53;

    if (!address.empty() && address.front() == '[') {
        const std::size_t close = address.find(']');
        if (close == std::string_view::npos) return Error{310, "malformed IPv6 endpoint"};
        const std::string_view literal = address.substr(1, close - 1);
        if (close + 1 < address.size()) {
            if (address[close + 1] != ':') return Error{310, "malformed endpoint"};
            auto parsed_port = parse_port(address.substr(close + 2));
            if (!parsed_port) return parsed_port.error();
            port = parsed_port.value();
        }
        auto parsed = IpAddress::parse(literal);
        if (!parsed) return parsed.error();
        return std::make_pair(parsed.value(), port);
    }

    const std::size_t colon = address.rfind(':');
    if (colon != std::string_view::npos && address.find(':') == colon) {
        auto parsed_port = parse_port(address.substr(colon + 1));
        if (!parsed_port) return parsed_port.error();
        port = parsed_port.value();
        address = address.substr(0, colon);
    }
    auto parsed = IpAddress::parse(address);
    if (!parsed) return parsed.error();
    return std::make_pair(parsed.value(), port);
}

asio::ip::address to_asio_address(const IpAddress& address) {
    if (address.v6) {
        asio::ip::address_v6::bytes_type bytes{};
        std::copy(std::begin(address.bytes), std::end(address.bytes), bytes.begin());
        return asio::ip::address_v6(bytes);
    }
    asio::ip::address_v4::bytes_type bytes{};
    std::copy(std::begin(address.bytes), std::begin(address.bytes) + 4, bytes.begin());
    return asio::ip::address_v4(bytes);
}

udp::endpoint to_udp_endpoint(const IpAddress& address, std::uint16_t port) {
    return udp::endpoint(to_asio_address(address), port);
}

tcp::endpoint to_tcp_endpoint(const IpAddress& address, std::uint16_t port) {
    return tcp::endpoint(to_asio_address(address), port);
}

bool is_unspecified(const IpAddress& address) {
    const std::size_t length = address.v6 ? 16 : 4;
    for (std::size_t i = 0; i < length; ++i) {
        if (address.bytes[i] != 0) return false;
    }
    return true;
}

IpAddress from_asio_address(const asio::ip::address& address) {
    IpAddress out;
    if (address.is_v6()) {
        const auto bytes = address.to_v6().to_bytes();
        std::copy(bytes.begin(), bytes.end(), out.bytes);
        out.v6 = true;
    } else {
        const auto bytes = address.to_v4().to_bytes();
        std::copy(bytes.begin(), bytes.end(), out.bytes);
    }
    return out;
}

IpAddress from_udp_endpoint(const udp::endpoint& endpoint) {
    return from_asio_address(endpoint.address());
}

}  // namespace

// ---------------------------------------------------------------------------
// UDP
// ---------------------------------------------------------------------------
class DnsServer::UdpListener : public std::enable_shared_from_this<UdpListener> {
public:
    UdpListener(asio::io_context& io, DnsServer& owner, udp::endpoint endpoint)
        : owner_(owner), socket_(io), endpoint_(std::move(endpoint)) {}

    Result<void> open() {
        asio::error_code ec;
        socket_.open(endpoint_.protocol(), ec);
        if (ec) return Error{311, "open failed: " + ec.message()};
        socket_.bind(endpoint_, ec);
        if (ec) return Error{312, "bind failed: " + ec.message()};
        return {};
    }

    void start() { receive(); }

    void stop() {
        asio::error_code ec;
        socket_.close(ec);
    }

    udp::endpoint local_endpoint() const { return socket_.local_endpoint(); }

private:
    void receive() {
        auto self = shared_from_this();
        // Buffer is per-receive so a slow handler cannot overwrite in-flight data.
        auto buffer = std::make_shared<std::array<std::uint8_t, 4096>>();
        auto sender = std::make_shared<udp::endpoint>();
        socket_.async_receive_from(
            asio::buffer(*buffer), *sender,
            [self, buffer, sender](const asio::error_code& ec, std::size_t bytes) {
                self->on_receive(ec, bytes, *buffer, *sender);
                // Keep serving after transient errors; stop only when the
                // socket was closed (operation_aborted) or a fatal error hit.
                if (ec != asio::error::operation_aborted && self->socket_.is_open()) {
                    self->receive();
                }
            });
    }

    void on_receive(const asio::error_code& ec, std::size_t bytes,
                    const std::array<std::uint8_t, 4096>& buffer, const udp::endpoint& sender) {
        // Datagrams larger than the receive buffer are reported as
        // error::message_size and dropped: DNS over UDP above 4096 bytes is
        // out of scope for v1 (clients must use TCP).
        if (ec) return;

        const IpAddress source = from_udp_endpoint(sender);
        if (!owner_.acl_.permits(source)) return;
        if (!owner_.limiter_.allow(source, now_ns())) return;

        QueryContext context;
        context.tcp = false;
        const auto parsed_query =
            dns::Message::parse(std::span<const std::uint8_t>(buffer.data(), bytes));
        if (parsed_query) {
            context.udp_payload =
                parsed_query.value().edns ? parsed_query.value().edns->udp_payload : 512;
        }

        auto response = owner_.handler_.handle(
            std::span<const std::uint8_t>(buffer.data(), bytes), context);
        if (!response) return;

        auto encoded = response->message.encode(context.udp_payload);
        std::vector<std::uint8_t> wire;
        if (encoded) {
            wire = std::move(encoded.value());
        } else {
            wire = response->message.encode_truncated();
        }
        send(std::move(wire), sender);
    }

    void send(std::vector<std::uint8_t> wire, const udp::endpoint& target) {
        auto self = shared_from_this();
        auto data = std::make_shared<std::vector<std::uint8_t>>(std::move(wire));
        socket_.async_send_to(asio::buffer(*data), target,
                              [self, data](const asio::error_code&, std::size_t) {});
    }

    DnsServer& owner_;
    udp::socket socket_;
    udp::endpoint endpoint_;
};

// ---------------------------------------------------------------------------
// TCP
// ---------------------------------------------------------------------------
class DnsServer::TcpListener : public std::enable_shared_from_this<TcpListener> {
public:
    TcpListener(asio::io_context& io, DnsServer& owner, tcp::endpoint endpoint)
        : owner_(owner), acceptor_(io), endpoint_(std::move(endpoint)) {}

    Result<void> open() {
        asio::error_code ec;
        acceptor_.open(endpoint_.protocol(), ec);
        if (ec) return Error{313, "open failed: " + ec.message()};
        acceptor_.set_option(tcp::acceptor::reuse_address(true), ec);
        acceptor_.bind(endpoint_, ec);
        if (ec) return Error{314, "bind failed: " + ec.message()};
        acceptor_.listen(asio::socket_base::max_listen_connections, ec);
        if (ec) return Error{315, "listen failed: " + ec.message()};
        return {};
    }

    void start() { accept(); }

    void stop() {
        asio::error_code ec;
        acceptor_.close(ec);
    }

    tcp::endpoint local_endpoint() const { return acceptor_.local_endpoint(); }

private:
    class Session : public std::enable_shared_from_this<Session> {
    public:
        Session(DnsServer& owner, tcp::socket socket, IpAddress peer,
                std::function<void()> on_close)
            : owner_(owner), socket_(std::move(socket)), peer_(peer),
              idle_timer_(socket_.get_executor()), on_close_(std::move(on_close)) {}

        ~Session() {
            if (on_close_) on_close_();
        }

        void start() {
            asio::error_code ec;
            socket_.set_option(tcp::no_delay(true), ec);
            arm_idle_timer();
            read_length();
        }

    private:
        void arm_idle_timer() {
            auto self = shared_from_this();
            idle_timer_.expires_after(std::chrono::seconds(owner_.config_.tcp_idle_seconds));
            idle_timer_.async_wait([self](const asio::error_code& ec) {
                if (!ec) {
                    asio::error_code close_ec;
                    self->socket_.close(close_ec);
                }
            });
        }

        void read_length() {
            auto self = shared_from_this();
            asio::async_read(socket_, asio::buffer(length_, 2),
                             [self](const asio::error_code& ec, std::size_t) {
                                 if (ec) return;
                                 self->read_payload();
                             });
        }

        void read_payload() {
            const std::size_t length =
                (static_cast<std::size_t>(length_[0]) << 8) | length_[1];
            if (length < dns::kHeaderSize) {
                close();  // framing violation: do not leave the connection open
                return;
            }

            auto self = shared_from_this();
            auto payload = std::make_shared<std::vector<std::uint8_t>>(length);
            asio::async_read(
                socket_, asio::buffer(*payload),
                [self, payload](const asio::error_code& ec, std::size_t) {
                    if (ec) return;
                    self->handle(*payload);
                });
        }

        void close() {
            idle_timer_.cancel();
            asio::error_code ec;
            socket_.close(ec);
        }

        void handle(const std::vector<std::uint8_t>& packet) {
            const IpAddress& source = peer_;
            if (!owner_.acl_.permits(source)) {
                close();
                return;
            }
            if (!owner_.limiter_.allow(source, now_ns())) {
                close();
                return;
            }

            auto response = owner_.handler_.handle(packet, QueryContext{true, 65535});
            if (!response) {
                close();
                return;
            }
            auto encoded = response->message.encode(65535);
            if (!encoded) {
                close();
                return;
            }

            const auto& wire = encoded.value();
            auto frame = std::make_shared<std::vector<std::uint8_t>>();
            frame->reserve(wire.size() + 2);
            frame->push_back(static_cast<std::uint8_t>(wire.size() >> 8));
            frame->push_back(static_cast<std::uint8_t>(wire.size() & 0xFF));
            frame->insert(frame->end(), wire.begin(), wire.end());

            auto self = shared_from_this();
            asio::async_write(socket_, asio::buffer(*frame),
                              [self, frame](const asio::error_code& ec, std::size_t) {
                                  if (ec) return;
                                  self->arm_idle_timer();
                                  self->read_length();
                              });
        }

        DnsServer& owner_;
        tcp::socket socket_;
        IpAddress peer_;
        asio::steady_timer idle_timer_;
        std::array<std::uint8_t, 2> length_{};
        std::function<void()> on_close_;
    };

    void accept() {
        auto self = shared_from_this();
        acceptor_.async_accept([self](const asio::error_code& ec, tcp::socket socket) {
            if (!ec) {
                // remote_endpoint() without an error_code throws when the peer
                // already reset the connection (port scans, health checks).
                // An exception escaping this handler would terminate the
                // process, so take the error_code overload and drop the socket.
                asio::error_code endpoint_ec;
                const auto remote = socket.remote_endpoint(endpoint_ec);
                if (endpoint_ec) {
                    asio::error_code ignored;
                    socket.close(ignored);
                    self->accept();
                    return;
                }
                const IpAddress peer = from_asio_address(remote.address());
                if (self->sessions_ < self->owner_.config_.max_tcp_connections) {
                    ++self->sessions_;
                    std::weak_ptr<TcpListener> weak = self;
                    std::make_shared<Session>(self->owner_, std::move(socket), peer, [weak] {
                        if (auto listener = weak.lock(); listener && listener->sessions_ > 0) {
                            --listener->sessions_;
                        }
                    })->start();
                } else {
                    asio::error_code ignored;
                    socket.close(ignored);
                }
            }
            // Re-arm unless the acceptor was closed. Errors such as EMFILE are
            // transient and must not spin: back off one second (PLAN M9 will
            // add metrics for this).
            if (ec == asio::error::operation_aborted || !self->acceptor_.is_open()) return;
            if (ec) {
                auto timer = std::make_shared<asio::steady_timer>(self->acceptor_.get_executor());
                timer->expires_after(std::chrono::seconds(1));
                timer->async_wait([self, timer](const asio::error_code& wait_ec) {
                    if (!wait_ec) self->accept();
                });
                return;
            }
            self->accept();
        });
    }

    DnsServer& owner_;
    tcp::acceptor acceptor_;
    tcp::endpoint endpoint_;
    std::size_t sessions_{0};
};

// ---------------------------------------------------------------------------
// DnsServer
// ---------------------------------------------------------------------------
DnsServer::DnsServer(asio::io_context& io, const QueryHandler& handler, ServerConfig config)
    : io_(io),
      handler_(handler),
      config_(std::move(config)),
      acl_(config_.allow_private_sources),
      limiter_(config_.rate_limit_qps, config_.rate_limit_burst) {
    for (const auto& cidr : config_.extra_allow) {
        (void)acl_.allow(cidr);
    }
    for (const auto& cidr : config_.deny) {
        (void)acl_.deny(cidr);
    }
}

DnsServer::~DnsServer() { stop(); }

Result<void> DnsServer::start() {
    if (started_) return Error{316, "server already started"};

    for (const auto& spec : config_.listen) {
        auto endpoint = parse_endpoint(spec);
        if (!endpoint) {
            stop();
            return Error{endpoint.error().code, spec + ": " + endpoint.error().message};
        }
        const auto [address, configured_port] = endpoint.value();
        if (is_unspecified(address)) {
            stop();
            return Error{317, spec + ": binding a wildcard address is not allowed (SPEC D14)"};
        }
        const udp::endpoint udp_endpoint = to_udp_endpoint(address, configured_port);

        std::shared_ptr<UdpListener> udp_listener;
        std::shared_ptr<TcpListener> tcp_listener;
        bool bound_ok = false;
        const int max_retries = configured_port == 0 ? 50 : 1;

        for (int attempt = 0; attempt < max_retries; ++attempt) {
            udp_listener = std::make_shared<UdpListener>(io_, *this, udp_endpoint);
            if (auto status = udp_listener->open(); !status) {
                udp_listener->stop();
                if (attempt + 1 == max_retries) {
                    stop();
                    return Error{status.error().code, spec + ": " + status.error().message};
                }
                continue;
            }

            // Port 0 asks the OS for an ephemeral port; TCP must use the same one,
            // otherwise UDP and TCP would listen on different ports.
            const std::uint16_t port =
                configured_port != 0 ? configured_port : udp_listener->local_endpoint().port();

            tcp_listener =
                std::make_shared<TcpListener>(io_, *this, to_tcp_endpoint(address, port));
            if (auto status = tcp_listener->open(); !status) {
                tcp_listener->stop();
                udp_listener->stop();
                if (attempt + 1 == max_retries) {
                    stop();
                    return Error{status.error().code, spec + ": " + status.error().message};
                }
                continue;
            }
            bound_ok = true;
            break;
        }

        if (!bound_ok) {
            stop();
            return Error{318, spec + ": failed to bind both UDP and TCP"};
        }

        udp_.push_back(std::move(udp_listener));
        tcp_.push_back(std::move(tcp_listener));
    }

    for (auto& listener : udp_) listener->start();
    for (auto& listener : tcp_) listener->start();
    started_ = true;
    return {};
}

void DnsServer::stop() {
    for (auto& listener : udp_) listener->stop();
    for (auto& listener : tcp_) listener->stop();
    udp_.clear();
    tcp_.clear();
    started_ = false;
}

std::vector<std::string> DnsServer::bound_addresses() const {
    std::vector<std::string> out;
    out.reserve(udp_.size());
    for (const auto& listener : udp_) {
        asio::error_code ec;
        const auto endpoint = listener->local_endpoint();
        if (ec) continue;
        out.push_back(endpoint.address().to_string() + ":" + std::to_string(endpoint.port()));
    }
    return out;
}

}  // namespace ldns
