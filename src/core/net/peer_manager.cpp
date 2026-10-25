#include "core/net/peer_manager.hpp"

#include <algorithm>

#include "core/domain/domain_service.hpp"
#include "core/net/crypto.hpp"
#include "core/server/acl.hpp"
#include "core/util/logger.hpp"

namespace ldns::net {

namespace {

std::pair<std::string, std::uint16_t> parse_endpoint(std::string_view ep, std::uint16_t default_port) {
    auto colon = ep.rfind(':');
    if (colon == std::string_view::npos) {
        return {std::string(ep), default_port};
    }
    // Check if IPv6 literal like [::1]:5391
    auto rbracket = ep.rfind(']');
    if (rbracket != std::string_view::npos && colon < rbracket) {
        return {std::string(ep), default_port};
    }
    std::string ip = std::string(ep.substr(0, colon));
    if (!ip.empty() && ip.front() == '[' && ip.back() == ']') {
        ip = ip.substr(1, ip.size() - 2);
    }
    std::uint16_t port = default_port;
    try {
        port = static_cast<std::uint16_t>(std::stoul(std::string(ep.substr(colon + 1))));
    } catch (...) {}
    return {ip, port};
}

}  // namespace

std::shared_ptr<PeerManager> PeerManager::create(
    asio::io_context& io, PeerManagerConfig config, PeerEventCallback on_event) {
    return std::shared_ptr<PeerManager>(
        new PeerManager(io, std::move(config), std::move(on_event)));
}

PeerManager::PeerManager(asio::io_context& io, PeerManagerConfig config,
                         PeerEventCallback on_event)
    : io_(io),
      config_(std::move(config)),
      on_event_(std::move(on_event)),
      acceptor_(io) {
    (void)crypto::init();
    net_hash_ = crypto::net_hash(config_.network_name);
}

PeerManager::~PeerManager() {
    stop();
}

Result<void> PeerManager::start() {
    if (running_) return {};

    asio::error_code ec;
    asio::ip::tcp::endpoint listen_ep(asio::ip::tcp::v4(), config_.sync_port);
    acceptor_.open(listen_ep.protocol(), ec);
    if (ec) {
        return Error{830, "failed to open peer sync acceptor: " + ec.message()};
    }

    acceptor_.set_option(asio::socket_base::reuse_address(true), ec);
    acceptor_.bind(listen_ep, ec);
    if (ec) {
        acceptor_.close();
        return Error{831, "failed to bind peer sync port " + std::to_string(config_.sync_port) +
                              ": " + ec.message()};
    }

    acceptor_.listen(asio::socket_base::max_listen_connections, ec);
    if (ec) {
        acceptor_.close();
        return Error{832, "failed to listen on sync port: " + ec.message()};
    }

    running_ = true;
    start_accept();

    // Process manual peers
    for (const auto& ep : config_.manual_peers) {
        (void)add_manual_peer(ep);
    }

    return {};
}

void PeerManager::stop() {
    if (!running_) return;
    running_ = false;

    asio::error_code ec;
    acceptor_.close(ec);

    std::vector<std::shared_ptr<Session>> to_close;
    {
        std::lock_guard lock(mutex_);
        for (auto& [_, sess] : active_sessions_) {
            to_close.push_back(sess);
        }
        active_sessions_.clear();
        backoff_timers_.clear();
    }

    for (auto& sess : to_close) {
        sess->close("peer manager stopped");
    }
}

std::uint16_t PeerManager::bound_port() const {
    asio::error_code ec;
    auto ep = acceptor_.local_endpoint(ec);
    if (ec) return 0;
    return ep.port();
}

void PeerManager::start_accept() {
    if (!running_ || !acceptor_.is_open()) return;

    auto self = shared_from_this();
    acceptor_.async_accept([this, self](const asio::error_code& ec, asio::ip::tcp::socket socket) {
        handle_accept(ec, std::move(socket));
    });
}

void PeerManager::handle_accept(const asio::error_code& ec, asio::ip::tcp::socket socket) {
    if (!running_ || ec == asio::error::operation_aborted) return;

    if (!ec) {
        std::lock_guard lock(mutex_);
        if (active_sessions_.size() >= kMaxPeersLimit) {
            // protocol §10: TOO_MANY_PEERS
            auto frame = encode_error_frame(
                ErrorMessage{ProtocolErrorCode::TooManyPeers, "max peers limit 64 reached"});
            if (frame) {
                asio::async_write(socket, asio::buffer(frame.value()),
                                  [s = std::move(socket)](const asio::error_code&, std::size_t) {});
            }
            start_accept();
            return;
        }

        SessionOptions s_opts;
        s_opts.my_node_id = config_.node_id;
        s_opts.my_name = config_.node_name;
        s_opts.net_hash = net_hash_;
        s_opts.sync_port = config_.sync_port;
        s_opts.dns_port = config_.dns_port;
        s_opts.my_ips = config_.my_ips;
        s_opts.my_epoch = config_.my_epoch;
        s_opts.my_seq = config_.my_seq;

        auto self = shared_from_this();
        auto session = Session::create_inbound(
            std::move(socket), std::move(s_opts),
            [self](std::shared_ptr<Session> s) { self->on_session_established(s); },
            [self](std::shared_ptr<Session> s, std::string_view r) { self->on_session_closed(s, r); },
            [self](std::shared_ptr<Session> s, const ParsedMessage& m) { self->on_session_message(s, m); });

        active_sessions_[session.get()] = session;
        session->start();
    }

    start_accept();
}

void PeerManager::on_beacon_discovered(const DiscoveredPeer& peer) {
    if (peer.node_id == config_.node_id) return;

    std::shared_ptr<Session> dial_to_start;
    std::string dial_addr;
    std::uint16_t dial_port{0};

    {
        std::lock_guard lock(mutex_);
        const std::string key = to_hex(peer.node_id);
        auto& record = peers_[key];
        record.node_id = peer.node_id;
        record.name = peer.name;
        record.sync_port = peer.sync_port;
        record.dns_port = peer.dns_port;
        record.ips = peer.ips;
        record.epoch = peer.epoch;
        record.seq = peer.seq;
        record.last_seen = std::chrono::steady_clock::now();

        if (peer.is_bye) {
            return;
        }

        if (record.is_online) {
            return;
        }

        // protocol §9.1: node_id(mình) < node_id(peer) -> mình dial
        const bool we_dial = (config_.node_id < peer.node_id);
        if (!we_dial && !record.is_manual) {
            return;
        }

        // Reset backoff on receiving fresh beacon (protocol §9.2)
        record.current_backoff = std::chrono::milliseconds(1000);
        record.next_dial_allowed = std::chrono::steady_clock::now();

        // Target address: prefer datagram source IP, then advertised ips (protocol §3.4)
        std::string target_ip = peer.source_endpoint.address().to_string();
        if (target_ip.empty() && !peer.ips.empty()) {
            target_ip = peer.ips.front();
        }
        if (target_ip.empty()) return;

        dial_addr = target_ip;
        dial_port = peer.sync_port;
    }

    dial_peer(peer.node_id, dial_addr, dial_port);
}

void PeerManager::dial_peer(const NodeId& node_id, const std::string& address, std::uint16_t port) {
    (void)node_id;
    asio::error_code ec;
    auto ip = asio::ip::make_address(address, ec);
    if (ec) return;

    asio::ip::tcp::endpoint target(ip, port);

    SessionOptions s_opts;
    s_opts.my_node_id = config_.node_id;
    s_opts.my_name = config_.node_name;
    s_opts.net_hash = net_hash_;
    s_opts.sync_port = config_.sync_port;
    s_opts.dns_port = config_.dns_port;
    s_opts.my_ips = config_.my_ips;
    s_opts.my_epoch = config_.my_epoch;
    s_opts.my_seq = config_.my_seq;

    auto self = shared_from_this();
    auto session = Session::create_outbound(
        io_, target, std::move(s_opts),
        [self](std::shared_ptr<Session> s) { self->on_session_established(s); },
        [self](std::shared_ptr<Session> s, std::string_view r) { self->on_session_closed(s, r); },
        [self](std::shared_ptr<Session> s, const ParsedMessage& m) { self->on_session_message(s, m); });

    {
        std::lock_guard lock(mutex_);
        active_sessions_[session.get()] = session;
    }

    session->start();
}

void PeerManager::on_session_established(std::shared_ptr<Session> session) {
    PeerRecord event_record;
    std::shared_ptr<Session> duplicate_to_close;

    {
        std::lock_guard lock(mutex_);
        const std::string key = to_hex(session->peer_node_id());
        auto& record = peers_[key];

        // Deduplication rule (protocol §9.1):
        // If an active session already exists for this peer:
        if (record.is_online && record.session && record.session != session) {
            // Keep the connection dialed by the smaller node_id!
            const bool we_are_smaller = (config_.node_id < session->peer_node_id());
            if (we_are_smaller) {
                // We dialed: keep our outbound session (role == Initiator)
                if (session->role() == SessionRole::Initiator) {
                    duplicate_to_close = record.session;
                    record.session = session;
                } else {
                    duplicate_to_close = session;
                }
            } else {
                // Peer dialed: keep peer's inbound session (role == Responder)
                if (session->role() == SessionRole::Responder) {
                    duplicate_to_close = record.session;
                    record.session = session;
                } else {
                    duplicate_to_close = session;
                }
            }
        } else {
            record.node_id = session->peer_node_id();
            record.name = session->peer_name();
            record.sync_port = session->peer_hello().sync_port;
            record.dns_port = session->peer_hello().dns_port;
            record.epoch = session->peer_hello().epoch;
            record.seq = session->peer_hello().seq;
            record.is_online = true;
            record.session = session;
            record.current_backoff = std::chrono::milliseconds(1000);
            event_record = record;
        }
    }

    if (duplicate_to_close) {
        duplicate_to_close->send_error(ProtocolErrorCode::Duplicate, "closing duplicate session");
        duplicate_to_close->close("duplicate connection closed per dial rule");
        return;
    }

    // Protocol §5.5: send PEERS exchange immediately after ESTABLISHED
    broadcast_peers();

    if (on_event_) {
        on_event_(event_record, true);
    }
}

void PeerManager::on_session_closed(std::shared_ptr<Session> session, std::string_view /*reason*/) {
    PeerRecord event_record;
    bool was_online{false};
    NodeId peer_id{};

    {
        std::lock_guard lock(mutex_);
        active_sessions_.erase(session.get());

        const std::string key = to_hex(session->peer_node_id());
        if (const auto it = peers_.find(key); it != peers_.end()) {
            if (it->second.session == session) {
                peer_id = it->second.node_id;
                was_online = it->second.is_online;
                it->second.is_online = false;
                it->second.session = nullptr;

                // Exponential backoff (protocol §9.2: 1s -> 2s -> 4s -> ... -> 60s)
                it->second.current_backoff = std::min(
                    it->second.current_backoff * 2, std::chrono::milliseconds(60000));
                it->second.next_dial_allowed = std::chrono::steady_clock::now() + it->second.current_backoff;

                event_record = it->second;
            }
        }
    }

    if (was_online && on_event_) {
        on_event_(event_record, false);
    }
}

void PeerManager::on_session_message(std::shared_ptr<Session> /*session*/, const ParsedMessage& msg) {
    if (msg.type == MessageType::Peers && msg.peers) {
        // Peer exchange: learn about new peers
        for (const auto& item : msg.peers->peers) {
            if (item.node_id == config_.node_id) continue;

            std::lock_guard lock(mutex_);
            const std::string key = to_hex(item.node_id);
            if (peers_.find(key) == peers_.end()) {
                auto& rec = peers_[key];
                rec.node_id = item.node_id;
                rec.sync_port = item.sync_port;
                for (const auto& ip : item.ips) {
                    const std::string text = domain::DomainService::format_address(ip);
                    if (!text.empty()) rec.ips.push_back(text);
                }
            }
        }
    }
}

void PeerManager::broadcast_peers() {
    std::vector<PeerItem> items;
    std::vector<std::shared_ptr<Session>> sessions;

    {
        std::lock_guard lock(mutex_);
        for (const auto& [_, p] : peers_) {
            if (p.is_online) {
                PeerItem item;
                item.node_id = p.node_id;
                item.sync_port = p.sync_port;
                for (const auto& ip_str : p.ips) {
                    const auto parsed = IpAddress::parse(ip_str);
                    if (parsed) {
                        const std::size_t sz = parsed.value().v6 ? 16 : 4;
                        item.ips.emplace_back(parsed.value().bytes, parsed.value().bytes + sz);
                    }
                }
                items.push_back(std::move(item));
            }
        }
        for (const auto& [_, sess] : active_sessions_) {
            if (sess->state() == SessionState::Established) {
                sessions.push_back(sess);
            }
        }
    }

    for (auto& sess : sessions) {
        sess->send_peers(items);
    }
}

Result<void> PeerManager::add_manual_peer(std::string_view endpoint) {
    auto [host, port] = parse_endpoint(endpoint, config_.sync_port);
    if (host.empty()) return Error{835, "invalid peer endpoint"};

    {
        std::lock_guard lock(mutex_);
        // If not known by node_id, use endpoint as temporary key
        std::string key = host + ":" + std::to_string(port);
        auto& rec = peers_[key];
        rec.name = host;
        rec.sync_port = port;
        rec.ips = {host};
        rec.is_manual = true;
    }

    dial_peer(NodeId{}, host, port);
    return {};
}

Result<void> PeerManager::remove_manual_peer(std::string_view endpoint) {
    auto [host, port] = parse_endpoint(endpoint, config_.sync_port);
    std::shared_ptr<Session> to_close;

    {
        std::lock_guard lock(mutex_);
        for (auto it = peers_.begin(); it != peers_.end();) {
            bool matches = false;
            for (const auto& ip : it->second.ips) {
                if (ip == host && it->second.sync_port == port) matches = true;
            }
            if (it->first == endpoint || matches) {
                if (it->second.session) to_close = it->second.session;
                it = peers_.erase(it);
            } else {
                ++it;
            }
        }
    }

    if (to_close) {
        to_close->close("manual peer removed");
    }
    return {};
}

std::vector<PeerRecord> PeerManager::list_peers() const {
    std::lock_guard lock(mutex_);
    std::vector<PeerRecord> out;
    out.reserve(peers_.size());
    for (const auto& [_, p] : peers_) {
        out.push_back(p);
    }
    return out;
}

std::optional<PeerRecord> PeerManager::get_peer(const NodeId& node_id) const {
    std::lock_guard lock(mutex_);
    const std::string key = to_hex(node_id);
    if (const auto it = peers_.find(key); it != peers_.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::size_t PeerManager::online_peer_count() const {
    std::lock_guard lock(mutex_);
    std::size_t count = 0;
    for (const auto& [_, p] : peers_) {
        if (p.is_online) ++count;
    }
    return count;
}

}  // namespace ldns::net
