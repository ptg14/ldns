#pragma once

#include <asio.hpp>
#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/net/beacon.hpp"
#include "core/net/crypto.hpp"
#include "core/util/id.hpp"
#include "core/util/result.hpp"

namespace ldns::net {

struct DiscoveryConfig {
    NodeId node_id{};
    std::string node_name;
    std::string network_name{"default"};
    std::uint16_t discovery_port{kDiscoveryPort};
    std::uint16_t sync_port{5391};
    std::uint16_t dns_port{53};
    std::string multicast_group{kDiscoveryMulticastIpv4};
    bool enable_broadcast{true};
    std::optional<std::array<std::uint8_t, 32>> k_beacon{std::nullopt};
};

struct DiscoveredPeer {
    NodeId node_id{};
    std::string name;
    std::uint16_t sync_port{5391};
    std::uint16_t dns_port{53};
    std::vector<std::string> ips;
    std::uint64_t epoch{0};
    std::uint64_t seq{0};
    asio::ip::udp::endpoint source_endpoint;
    bool is_bye{false};
};

using PeerDiscoveredCallback = std::function<void(const DiscoveredPeer&)>;

class DiscoveryService {
public:
    DiscoveryService(asio::io_context& io, DiscoveryConfig config,
                     PeerDiscoveredCallback callback);
    ~DiscoveryService();

    DiscoveryService(const DiscoveryService&) = delete;
    DiscoveryService& operator=(const DiscoveryService&) = delete;

    Result<void> start();
    void stop();

    // Broadcasts an updated beacon immediately (rate limited to max 1/sec).
    void trigger_beacon(std::uint64_t epoch, std::uint64_t seq,
                        const std::vector<std::vector<std::uint8_t>>& ips);

    // Sends graceful exit 'bye' beacons (3 datagrams spaced 100ms apart per protocol §3.3).
    void send_bye(std::function<void()> on_complete = nullptr);

    bool is_running() const noexcept { return running_; }
    std::uint16_t bound_port() const;

private:
    void start_receive();
    void handle_receive(const asio::error_code& ec, std::size_t bytes_transferred);
    void schedule_beacon_timer();
    void send_beacon_now(bool is_bye);

    asio::io_context& io_;
    DiscoveryConfig config_;
    std::array<std::uint8_t, 8> net_hash_{};
    PeerDiscoveredCallback callback_;

    asio::ip::udp::socket socket_;
    asio::steady_timer timer_;
    asio::steady_timer bye_timer_;

    std::array<std::uint8_t, 2048> recv_buffer_{};
    asio::ip::udp::endpoint remote_endpoint_{};

    bool running_{false};
    std::chrono::steady_clock::time_point start_time_;
    std::chrono::steady_clock::time_point last_beacon_time_;

    std::uint64_t current_epoch_{0};
    std::uint64_t current_seq_{0};
    std::vector<std::vector<std::uint8_t>> current_ips_;

    int bye_count_{0};
    std::function<void()> bye_completion_callback_;
};

}  // namespace ldns::net
