#pragma once

#include <asio.hpp>
#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/net/discovery.hpp"
#include "core/net/protocol_message.hpp"
#include "core/net/session.hpp"
#include "core/util/id.hpp"
#include "core/util/result.hpp"

namespace ldns::net {

struct PeerRecord {
    NodeId node_id{};
    std::string name;
    std::vector<std::string> ips;
    std::uint16_t sync_port{5391};
    std::uint16_t dns_port{53};
    std::uint64_t epoch{0};
    std::uint64_t seq{0};
    bool is_online{false};
    bool is_manual{false};
    std::chrono::steady_clock::time_point last_seen{};
    std::chrono::milliseconds current_backoff{1000};
    std::chrono::steady_clock::time_point next_dial_allowed{};
    std::shared_ptr<Session> session{nullptr};
};

struct PeerManagerConfig {
    NodeId node_id{};
    std::string node_name;
    std::string network_name{"default"};
    std::uint16_t sync_port{5391};
    std::uint16_t dns_port{53};
    std::vector<std::string> listen_addresses{"0.0.0.0"};
    std::vector<std::string> manual_peers;
    std::vector<std::vector<std::uint8_t>> my_ips;
    std::uint64_t my_epoch{0};
    std::uint64_t my_seq{0};
};

class PeerManager : public std::enable_shared_from_this<PeerManager> {
public:
    using PeerEventCallback = std::function<void(const PeerRecord&, bool online)>;

    static std::shared_ptr<PeerManager> create(
        asio::io_context& io, PeerManagerConfig config,
        PeerEventCallback on_event = nullptr);

    ~PeerManager();

    Result<void> start();
    void stop();

    // Invoked when DiscoveryService discovers a peer beacon
    void on_beacon_discovered(const DiscoveredPeer& peer);

    // Manual peer management (CLI / config)
    Result<void> add_manual_peer(std::string_view endpoint);
    Result<void> remove_manual_peer(std::string_view endpoint);

    std::vector<PeerRecord> list_peers() const;
    std::optional<PeerRecord> get_peer(const NodeId& node_id) const;
    std::size_t online_peer_count() const;

    std::uint16_t bound_port() const;

    using SyncMessageHandler = std::function<void(std::shared_ptr<Session>, const ParsedMessage&)>;
    void set_sync_handler(SyncMessageHandler handler);
    void broadcast_frame(std::vector<std::uint8_t> frame, const NodeId* exclude_peer1 = nullptr,
                         const NodeId* exclude_peer2 = nullptr);

private:
    PeerManager(asio::io_context& io, PeerManagerConfig config, PeerEventCallback on_event);

    void start_accept();
    void handle_accept(const asio::error_code& ec, asio::ip::tcp::socket socket);

    void dial_peer(const NodeId& node_id, const std::string& address, std::uint16_t port);
    void schedule_backoff_timer(const NodeId& node_id);

    void on_session_established(std::shared_ptr<Session> session);
    void on_session_closed(std::shared_ptr<Session> session, std::string_view reason);
    void on_session_message(std::shared_ptr<Session> session, const ParsedMessage& msg);

    void broadcast_peers();

    asio::io_context& io_;
    PeerManagerConfig config_;
    PeerEventCallback on_event_;
    SyncMessageHandler sync_handler_;
    std::array<std::uint8_t, 8> net_hash_{};

    asio::ip::tcp::acceptor acceptor_;
    bool running_{false};

    mutable std::mutex mutex_;
    // Keyed by hex string of NodeId
    std::unordered_map<std::string, PeerRecord> peers_;
    // Sessions keyed by raw pointer or session ID
    std::unordered_map<const Session*, std::shared_ptr<Session>> active_sessions_;

    std::unordered_map<std::string, std::unique_ptr<asio::steady_timer>> backoff_timers_;
};

}  // namespace ldns::net
