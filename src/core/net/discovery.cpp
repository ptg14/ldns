#include "core/net/discovery.hpp"

#include <random>

#include "core/domain/domain_service.hpp"
#include "core/util/logger.hpp"

namespace ldns::net {

namespace {

int random_jitter_ms(int base_ms, double fraction) {
    static thread_local std::mt19937 gen{std::random_device{}()};
    const int delta = static_cast<int>(base_ms * fraction);
    std::uniform_int_distribution<int> dist(-delta, delta);
    return base_ms + dist(gen);
}

}  // namespace

DiscoveryService::DiscoveryService(asio::io_context& io, DiscoveryConfig config,
                                   PeerDiscoveredCallback callback)
    : io_(io),
      config_(std::move(config)),
      callback_(std::move(callback)),
      socket_(io),
      timer_(io),
      bye_timer_(io) {
    (void)crypto::init();
    net_hash_ = crypto::net_hash(config_.network_name);
}

DiscoveryService::~DiscoveryService() {
    stop();
}

Result<void> DiscoveryService::start() {
    if (running_) return {};

    asio::error_code ec;
    socket_.open(asio::ip::udp::v4(), ec);
    if (ec) {
        return Error{810, "failed to open discovery socket: " + ec.message()};
    }

    socket_.set_option(asio::socket_base::reuse_address(true), ec);
    socket_.set_option(asio::socket_base::broadcast(true), ec);

    asio::ip::udp::endpoint listen_ep(asio::ip::address_v4::any(), config_.discovery_port);
    socket_.bind(listen_ep, ec);
    if (ec) {
        socket_.close();
        return Error{811, "failed to bind discovery port " + std::to_string(config_.discovery_port) +
                              ": " + ec.message()};
    }

    // Join multicast group
    if (!config_.multicast_group.empty()) {
        asio::error_code mc_ec;
        auto mc_addr = asio::ip::make_address_v4(config_.multicast_group, mc_ec);
        if (!mc_ec) {
            socket_.set_option(asio::ip::multicast::join_group(mc_addr), mc_ec);
            socket_.set_option(asio::ip::multicast::hops(1), mc_ec);
        }
    }

    running_ = true;
    start_time_ = std::chrono::steady_clock::now();

    start_receive();
    send_beacon_now(false);
    schedule_beacon_timer();

    return {};
}

void DiscoveryService::stop() {
    if (!running_) return;
    running_ = false;

    timer_.cancel();
    bye_timer_.cancel();
    if (socket_.is_open()) {
        asio::error_code ec;
        socket_.close(ec);
    }
}

std::uint16_t DiscoveryService::bound_port() const {
    asio::error_code ec;
    auto ep = socket_.local_endpoint(ec);
    if (ec) return 0;
    return ep.port();
}

void DiscoveryService::trigger_beacon(std::uint64_t epoch, std::uint64_t seq,
                                      const std::vector<std::vector<std::uint8_t>>& ips) {
    current_epoch_ = epoch;
    current_seq_ = seq;
    current_ips_ = ips;

    if (!running_) return;

    // Rate limit: at most 1 beacon per second when triggered dynamically (protocol §3.3)
    const auto now = std::chrono::steady_clock::now();
    if (now - last_beacon_time_ < std::chrono::seconds(1)) {
        return;
    }
    send_beacon_now(false);
}

void DiscoveryService::send_beacon_now(bool is_bye) {
    if (!socket_.is_open()) return;

    Beacon b;
    b.type = is_bye ? "bye" : "beacon";
    b.version = {1, 0};
    b.node_id = config_.node_id;
    b.name = config_.node_name;
    b.net = net_hash_;
    b.sync_port = config_.sync_port;
    b.dns_port = config_.dns_port;
    b.ips = current_ips_;
    b.epoch = current_epoch_;
    b.seq = current_seq_;
    b.ts = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());

    std::span<const std::uint8_t> k_b;
    if (config_.k_beacon) {
        k_b = *config_.k_beacon;
    }
    auto enc = encode_beacon(b, k_b);
    if (!enc) return;

    auto buf = std::make_shared<std::vector<std::uint8_t>>(std::move(enc.value()));
    last_beacon_time_ = std::chrono::steady_clock::now();

    // 1. Multicast endpoint
    if (!config_.multicast_group.empty()) {
        asio::error_code mc_ec;
        auto mc_addr = asio::ip::make_address_v4(config_.multicast_group, mc_ec);
        if (!mc_ec) {
            asio::ip::udp::endpoint mc_ep(mc_addr, config_.discovery_port);
            socket_.async_send_to(asio::buffer(*buf), mc_ep,
                                  [buf](const asio::error_code&, std::size_t) {});
        }
    }

    // 2. Broadcast endpoint (fallback per protocol §3.3)
    if (config_.enable_broadcast) {
        asio::ip::udp::endpoint bc_ep(asio::ip::address_v4::broadcast(), config_.discovery_port);
        socket_.async_send_to(asio::buffer(*buf), bc_ep,
                              [buf](const asio::error_code&, std::size_t) {});
    }
}

void DiscoveryService::schedule_beacon_timer() {
    if (!running_) return;

    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = now - start_time_;

    int next_interval_ms = 10000;
    if (elapsed < std::chrono::seconds(10)) {
        // Every 2s in the first 10s (protocol §3.3)
        next_interval_ms = 2000;
    } else {
        // Every 10s ± 20% jitter (protocol §3.3)
        next_interval_ms = random_jitter_ms(10000, 0.20);
    }

    timer_.expires_after(std::chrono::milliseconds(next_interval_ms));
    timer_.async_wait([this](const asio::error_code& ec) {
        if (ec || !running_) return;
        send_beacon_now(false);
        schedule_beacon_timer();
    });
}

void DiscoveryService::send_bye(std::function<void()> on_complete) {
    if (!running_ || !socket_.is_open()) {
        if (on_complete) on_complete();
        return;
    }

    bye_completion_callback_ = std::move(on_complete);
    bye_count_ = 0;

    auto send_next_bye = [this, self_fn = std::function<void()>()](auto& recurse) mutable -> void {
        if (!running_ || !socket_.is_open() || bye_count_ >= 3) {
            if (bye_completion_callback_) {
                auto cb = std::move(bye_completion_callback_);
                cb();
            }
            return;
        }

        send_beacon_now(true);
        bye_count_++;

        if (bye_count_ < 3) {
            bye_timer_.expires_after(std::chrono::milliseconds(100));
            bye_timer_.async_wait([&recurse](const asio::error_code& ec) {
                if (ec) return;
                recurse(recurse);
            });
        } else {
            if (bye_completion_callback_) {
                auto cb = std::move(bye_completion_callback_);
                cb();
            }
        }
    };

    send_next_bye(send_next_bye);
}

void DiscoveryService::start_receive() {
    if (!running_ || !socket_.is_open()) return;

    socket_.async_receive_from(
        asio::buffer(recv_buffer_), remote_endpoint_,
        [this](const asio::error_code& ec, std::size_t bytes_transferred) {
            handle_receive(ec, bytes_transferred);
        });
}

void DiscoveryService::handle_receive(const asio::error_code& ec, std::size_t bytes_transferred) {
    if (!running_ || ec == asio::error::operation_aborted) return;

    if (!ec && bytes_transferred > 0) {
        std::span<const std::uint8_t> k_b;
        if (config_.k_beacon) {
            k_b = *config_.k_beacon;
        }
        auto dec = decode_beacon(
            std::span<const std::uint8_t>(recv_buffer_.data(), bytes_transferred), k_b);
        if (dec) {
            const auto& b = dec.value();
            // Drop packets from ourselves (protocol §3.4)
            const bool from_self = (b.node_id == config_.node_id);
            // Drop packets from other networks
            const bool matching_net = (b.net == net_hash_);
            // Drop packets with different major version
            const bool matching_ver = (b.version[0] == 1);

            if (!from_self && matching_net && matching_ver) {
                DiscoveredPeer peer;
                peer.node_id = b.node_id;
                peer.name = b.name;
                peer.sync_port = b.sync_port;
                peer.dns_port = b.dns_port;
                peer.epoch = b.epoch;
                peer.seq = b.seq;
                peer.source_endpoint = remote_endpoint_;
                peer.is_bye = (b.type == "bye");

                for (const auto& ip_bytes : b.ips) {
                    const std::string text = domain::DomainService::format_address(ip_bytes);
                    if (!text.empty()) peer.ips.push_back(text);
                }

                if (callback_) {
                    callback_(peer);
                }
            }
        }
    }

    start_receive();
}

}  // namespace ldns::net
