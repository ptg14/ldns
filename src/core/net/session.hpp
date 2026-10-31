#pragma once

#include <asio.hpp>
#include <array>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/net/protocol_message.hpp"
#include "core/util/id.hpp"
#include "core/util/result.hpp"

namespace ldns::net {

enum class SessionState {
    Connecting,
    Handshaking,
    Established,
    Closed,
};

enum class SessionRole {
    Initiator,
    Responder,
};

struct SessionOptions {
    NodeId my_node_id{};
    std::string my_name;
    std::array<std::uint8_t, 8> net_hash{};
    std::uint16_t sync_port{5391};
    std::uint16_t dns_port{53};
    std::vector<std::vector<std::uint8_t>> my_ips;
    std::uint64_t my_epoch{0};
    std::uint64_t my_seq{0};
    std::chrono::milliseconds handshake_timeout{5000};
    std::chrono::milliseconds ping_interval{5000};
    std::chrono::milliseconds ping_timeout{15000};
    std::optional<std::array<std::uint8_t, 32>> k_root;
    std::optional<std::array<std::uint8_t, 32>> k_auth;
};

class Session : public std::enable_shared_from_this<Session> {
public:
    using EstablishedHandler = std::function<void(std::shared_ptr<Session>)>;
    using ClosedHandler = std::function<void(std::shared_ptr<Session>, std::string_view reason)>;
    using MessageHandler = std::function<void(std::shared_ptr<Session>, const ParsedMessage&)>;

    // Creates an inbound session from an accepted socket
    static std::shared_ptr<Session> create_inbound(
        asio::ip::tcp::socket socket, SessionOptions options,
        EstablishedHandler on_est, ClosedHandler on_close, MessageHandler on_msg);

    // Creates an outbound session to dial a remote endpoint
    static std::shared_ptr<Session> create_outbound(
        asio::io_context& io, asio::ip::tcp::endpoint target, SessionOptions options,
        EstablishedHandler on_est, ClosedHandler on_close, MessageHandler on_msg);

    ~Session();

    void start();
    void close(std::string_view reason);

    void send_frame(std::vector<std::uint8_t> framed_bytes);
    void send_ping();
    void send_pong();
    void send_bye();
    void send_error(ProtocolErrorCode code, std::string msg);
    void send_error_and_close(ProtocolErrorCode code, std::string msg);
    void send_peers(const std::vector<PeerItem>& peers);

    SessionState state() const noexcept { return state_; }
    SessionRole role() const noexcept { return role_; }
    const NodeId& peer_node_id() const noexcept { return peer_node_id_; }
    const std::string& peer_name() const noexcept { return peer_name_; }
    const HelloMessage& peer_hello() const noexcept { return peer_hello_; }
    asio::ip::tcp::endpoint remote_endpoint() const noexcept { return remote_endpoint_; }

private:
    Session(asio::ip::tcp::socket socket, SessionRole role,
            SessionOptions options, EstablishedHandler on_est,
            ClosedHandler on_close, MessageHandler on_msg,
            std::optional<asio::ip::tcp::endpoint> dial_target = std::nullopt);

    void do_connect(const asio::ip::tcp::endpoint& target);
    void do_handshake_send_hello();
    void send_unencrypted_frame(std::vector<std::uint8_t> framed_bytes);
    void start_read_length();
    void handle_read_length(const asio::error_code& ec, std::size_t bytes);
    void handle_read_payload(const asio::error_code& ec, std::size_t bytes);
    void handle_incoming_message(const ParsedMessage& msg, std::span<const std::uint8_t> raw_payload);

    void start_heartbeat();
    void schedule_ping();
    void reset_activity_timer();
    void check_activity_timeout(const asio::error_code& ec);

    void do_write();

    asio::ip::tcp::socket socket_;
    SessionRole role_;
    SessionOptions options_;
    EstablishedHandler on_established_;
    ClosedHandler on_closed_;
    MessageHandler on_message_;
    std::optional<asio::ip::tcp::endpoint> dial_target_;

    SessionState state_{SessionState::Connecting};
    NodeId peer_node_id_{};
    std::string peer_name_;
    HelloMessage peer_hello_{};
    asio::ip::tcp::endpoint remote_endpoint_{};

    asio::steady_timer handshake_timer_;
    asio::steady_timer ping_timer_;
    asio::steady_timer activity_timer_;

    std::array<std::uint8_t, 4> length_buffer_{};
    std::vector<std::uint8_t> payload_buffer_;
    std::deque<std::vector<std::uint8_t>> write_queue_;

    bool aead_active_{false};
    std::array<std::uint8_t, 32> send_key_{};
    std::array<std::uint8_t, 32> recv_key_{};
    std::array<std::uint8_t, 32> th_{};
    std::uint64_t send_counter_{0};
    std::uint64_t recv_counter_{0};
    std::vector<std::uint8_t> raw_my_hello_;
    std::vector<std::uint8_t> raw_peer_hello_;
    std::chrono::steady_clock::time_point update_rate_window_start_{};
    std::size_t update_ops_in_window_{0};

    bool closed_{false};
};

}  // namespace ldns::net
