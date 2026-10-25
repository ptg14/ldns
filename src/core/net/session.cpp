#include "core/net/session.hpp"

#include "core/util/logger.hpp"

namespace ldns::net {

std::shared_ptr<Session> Session::create_inbound(
    asio::ip::tcp::socket socket, SessionOptions options,
    EstablishedHandler on_est, ClosedHandler on_close, MessageHandler on_msg) {
    auto session = std::shared_ptr<Session>(new Session(
        std::move(socket), SessionRole::Responder, std::move(options),
        std::move(on_est), std::move(on_close), std::move(on_msg)));

    asio::error_code ec;
    session->remote_endpoint_ = session->socket_.remote_endpoint(ec);
    return session;
}

std::shared_ptr<Session> Session::create_outbound(
    asio::io_context& io, asio::ip::tcp::endpoint target, SessionOptions options,
    EstablishedHandler on_est, ClosedHandler on_close, MessageHandler on_msg) {
    asio::ip::tcp::socket socket(io);
    auto session = std::shared_ptr<Session>(new Session(
        std::move(socket), SessionRole::Initiator, std::move(options),
        std::move(on_est), std::move(on_close), std::move(on_msg), target));
    session->remote_endpoint_ = target;
    return session;
}

Session::Session(asio::ip::tcp::socket socket, SessionRole role,
                 SessionOptions options, EstablishedHandler on_est,
                 ClosedHandler on_close, MessageHandler on_msg,
                 std::optional<asio::ip::tcp::endpoint> dial_target)
    : socket_(std::move(socket)),
      role_(role),
      options_(std::move(options)),
      on_established_(std::move(on_est)),
      on_closed_(std::move(on_close)),
      on_message_(std::move(on_msg)),
      dial_target_(dial_target),
      handshake_timer_(socket_.get_executor()),
      ping_timer_(socket_.get_executor()),
      activity_timer_(socket_.get_executor()) {}

Session::~Session() {
    close("destructed");
}

void Session::start() {
    if (role_ == SessionRole::Initiator && dial_target_) {
        state_ = SessionState::Connecting;
        do_connect(*dial_target_);
    } else {
        state_ = SessionState::Handshaking;
        // Start 5-second handshake timer (protocol §5.1)
        handshake_timer_.expires_after(options_.handshake_timeout);
        auto self = shared_from_this();
        handshake_timer_.async_wait([self](const asio::error_code& ec) {
            if (!ec && self->state_ == SessionState::Handshaking) {
                self->close("handshake timed out");
            }
        });
        start_read_length();
    }
}

void Session::do_connect(const asio::ip::tcp::endpoint& target) {
    auto self = shared_from_this();
    socket_.async_connect(target, [self](const asio::error_code& ec) {
        if (ec) {
            self->close("connect failed: " + ec.message());
            return;
        }

        asio::error_code ep_ec;
        self->remote_endpoint_ = self->socket_.remote_endpoint(ep_ec);
        self->state_ = SessionState::Handshaking;

        // Start 5s handshake timer
        self->handshake_timer_.expires_after(self->options_.handshake_timeout);
        self->handshake_timer_.async_wait([self](const asio::error_code& timer_ec) {
            if (!timer_ec && self->state_ == SessionState::Handshaking) {
                self->close("handshake timed out");
            }
        });

        // Initiator sends HELLO first (protocol §5.1)
        self->do_handshake_send_hello();
        self->start_read_length();
    });
}

void Session::do_handshake_send_hello() {
    HelloMessage h;
    h.version = {1, 0};
    h.node_id = options_.my_node_id;
    h.name = options_.my_name;
    h.net = options_.net_hash;
    h.epoch = options_.my_epoch;
    h.seq = options_.my_seq;
    h.ips = options_.my_ips;
    h.sync_port = options_.sync_port;
    h.dns_port = options_.dns_port;
    h.caps = {"relay", "snapshot-chunk"};
    h.nonce = {0};
    h.psk = false;

    auto frame = encode_hello_frame(h);
    if (frame) {
        send_frame(std::move(frame.value()));
    }
}

void Session::close(std::string_view reason) {
    if (closed_) return;
    closed_ = true;
    state_ = SessionState::Closed;

    handshake_timer_.cancel();
    ping_timer_.cancel();
    activity_timer_.cancel();

    asio::error_code ec;
    if (socket_.is_open()) {
        socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
        socket_.close(ec);
    }

    if (on_closed_) {
        auto cb = std::move(on_closed_);
        cb(shared_from_this(), reason);
    }
}

void Session::send_frame(std::vector<std::uint8_t> framed_bytes) {
    if (closed_ || !socket_.is_open()) return;

    auto self = shared_from_this();
    asio::post(socket_.get_executor(), [self, bytes = std::move(framed_bytes)]() mutable {
        const bool write_in_progress = !self->write_queue_.empty();
        self->write_queue_.push_back(std::move(bytes));
        if (!write_in_progress) {
            self->do_write();
        }
    });
}

void Session::do_write() {
    if (closed_ || write_queue_.empty() || !socket_.is_open()) return;

    auto self = shared_from_this();
    asio::async_write(
        socket_, asio::buffer(write_queue_.front()),
        [self](const asio::error_code& ec, std::size_t) {
            if (ec) {
                self->close("write failed: " + ec.message());
                return;
            }
            self->write_queue_.pop_front();
            if (!self->write_queue_.empty()) {
                self->do_write();
            }
        });
}

void Session::send_ping() {
    auto frame = encode_ping_frame();
    if (frame) send_frame(std::move(frame.value()));
}

void Session::send_pong() {
    auto frame = encode_pong_frame();
    if (frame) send_frame(std::move(frame.value()));
}

void Session::send_bye() {
    auto frame = encode_bye_frame();
    if (frame) send_frame(std::move(frame.value()));
}

void Session::send_error(ProtocolErrorCode code, std::string msg) {
    auto frame = encode_error_frame(ErrorMessage{code, std::move(msg)});
    if (frame) send_frame(std::move(frame.value()));
}

void Session::send_peers(const std::vector<PeerItem>& peers) {
    auto frame = encode_peers_frame(PeersMessage{peers});
    if (frame) send_frame(std::move(frame.value()));
}

void Session::start_read_length() {
    if (closed_ || !socket_.is_open()) return;

    auto self = shared_from_this();
    asio::async_read(
        socket_, asio::buffer(length_buffer_),
        [self](const asio::error_code& ec, std::size_t bytes) {
            self->handle_read_length(ec, bytes);
        });
}

void Session::handle_read_length(const asio::error_code& ec, std::size_t bytes) {
    if (ec) {
        close(ec == asio::error::eof ? "connection closed by peer" : "read error: " + ec.message());
        return;
    }
    if (bytes != 4) {
        close("unexpected short length prefix");
        return;
    }

    const std::uint32_t len = (static_cast<std::uint32_t>(length_buffer_[0]) << 24) |
                              (static_cast<std::uint32_t>(length_buffer_[1]) << 16) |
                              (static_cast<std::uint32_t>(length_buffer_[2]) << 8) |
                              static_cast<std::uint32_t>(length_buffer_[3]);

    if (len > kMaxFrameSize) {
        send_error(ProtocolErrorCode::Malformed, "frame size exceeds 4 MiB limit");
        close("frame length exceeds 4 MiB limit");
        return;
    }

    payload_buffer_.resize(len);
    auto self = shared_from_this();
    asio::async_read(
        socket_, asio::buffer(payload_buffer_),
        [self](const asio::error_code& read_ec, std::size_t read_bytes) {
            self->handle_read_payload(read_ec, read_bytes);
        });
}

void Session::handle_read_payload(const asio::error_code& ec, std::size_t bytes) {
    if (ec) {
        close("read payload failed: " + ec.message());
        return;
    }

    reset_activity_timer();

    auto parsed = parse_payload(std::span<const std::uint8_t>(payload_buffer_.data(), bytes));
    if (!parsed) {
        send_error(ProtocolErrorCode::Malformed, "failed to parse messagepack");
        close("failed to parse messagepack: " + parsed.error().message);
        return;
    }

    handle_incoming_message(parsed.value());

    if (!closed_) {
        start_read_length();
    }
}

void Session::handle_incoming_message(const ParsedMessage& msg) {
    if (state_ == SessionState::Handshaking) {
        if (msg.type != MessageType::Hello || !msg.hello) {
            send_error(ProtocolErrorCode::Malformed, "expected HELLO during handshake");
            close("expected HELLO during handshake");
            return;
        }

        const auto& h = msg.hello.value();

        // 1. Check version major (protocol §5.2)
        if (h.version[0] != 1) {
            send_error(ProtocolErrorCode::Version, "unsupported version major");
            close("version mismatch");
            return;
        }

        // 2. Check network hash (protocol §5.2)
        if (h.net != options_.net_hash) {
            send_error(ProtocolErrorCode::Network, "network mismatch");
            close("network mismatch");
            return;
        }

        // 3. Check self connection (protocol §5.2)
        if (h.node_id == options_.my_node_id) {
            send_error(ProtocolErrorCode::Self, "connected to self");
            close("connected to self");
            return;
        }

        // 4. Check PSK flag (protocol §5.2)
        if (h.psk != false) {
            send_error(ProtocolErrorCode::Auth, "psk configuration mismatch");
            close("psk mismatch");
            return;
        }

        peer_node_id_ = h.node_id;
        peer_name_ = h.name;
        peer_hello_ = h;

        // If responder, send our HELLO back now (protocol §5.1)
        if (role_ == SessionRole::Responder) {
            do_handshake_send_hello();
        }

        handshake_timer_.cancel();
        state_ = SessionState::Established;

        start_heartbeat();

        if (on_established_) {
            on_established_(shared_from_this());
        }
        return;
    }

    if (state_ == SessionState::Established) {
        switch (msg.type) {
            case MessageType::Ping:
                send_pong();
                break;
            case MessageType::Pong:
                // Handled implicitly by reset_activity_timer()
                break;
            case MessageType::Bye:
                close("received BYE");
                break;
            case MessageType::Error:
                close("peer sent error: " + (msg.error ? msg.error->msg : ""));
                break;
            default:
                if (on_message_) {
                    on_message_(shared_from_this(), msg);
                }
                break;
        }
    }
}

void Session::start_heartbeat() {
    reset_activity_timer();
    schedule_ping();
}

void Session::schedule_ping() {
    if (closed_ || state_ != SessionState::Established) return;

    ping_timer_.expires_after(options_.ping_interval);
    auto self = shared_from_this();
    ping_timer_.async_wait([self](const asio::error_code& ec) {
        if (!ec && self->state_ == SessionState::Established) {
            self->send_ping();
            self->schedule_ping();
        }
    });
}

void Session::reset_activity_timer() {
    activity_timer_.expires_after(options_.ping_timeout);
    auto self = shared_from_this();
    activity_timer_.async_wait([self](const asio::error_code& ec) {
        self->check_activity_timeout(ec);
    });
}

void Session::check_activity_timeout(const asio::error_code& ec) {
    if (!ec && state_ == SessionState::Established) {
        close("heartbeat timeout (15s inactivity)");
    }
}

}  // namespace ldns::net
