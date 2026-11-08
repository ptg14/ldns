#include "core/resolver/upstream_client.hpp"

#include <array>
#include <chrono>
#include <cstring>
#include <asio.hpp>

namespace ldns::resolver {
namespace {

using asio::ip::tcp;
using asio::ip::udp;

asio::ip::address to_asio(const IpAddress& address) {
    if (address.v6) {
        asio::ip::address_v6::bytes_type bytes{};
        std::memcpy(bytes.data(), address.bytes, 16);
        return asio::ip::address_v6(bytes);
    }
    asio::ip::address_v4::bytes_type bytes{};
    std::memcpy(bytes.data(), address.bytes, 4);
    return asio::ip::address_v4(bytes);
}

Result<std::pair<IpAddress, std::uint16_t>> parse_endpoint(std::string_view text) {
    std::string_view address = text;
    std::uint16_t port = 53;

    if (!address.empty() && address.front() == '[') {
        const std::size_t close = address.find(']');
        if (close == std::string_view::npos) return Error{310, "malformed IPv6 endpoint"};
        const std::string_view literal = address.substr(1, close - 1);
        if (close + 1 < address.size()) {
            if (address[close + 1] != ':') return Error{310, "malformed endpoint"};
            int val = 0;
            for (char c : address.substr(close + 2)) {
                if (c < '0' || c > '9') return Error{310, "invalid port"};
                val = val * 10 + (c - '0');
            }
            if (val > 65535) return Error{310, "port out of range"};
            port = static_cast<std::uint16_t>(val);
        }
        auto parsed = IpAddress::parse(literal);
        if (!parsed) return parsed.error();
        return std::make_pair(parsed.value(), port);
    }

    const std::size_t colon = address.rfind(':');
    if (colon != std::string_view::npos && address.find(':') == colon) {
        int val = 0;
        for (char c : address.substr(colon + 1)) {
            if (c < '0' || c > '9') return Error{310, "invalid port"};
            val = val * 10 + (c - '0');
        }
        if (val > 65535) return Error{310, "port out of range"};
        port = static_cast<std::uint16_t>(val);
        address = address.substr(0, colon);
    }
    auto parsed = IpAddress::parse(address);
    if (!parsed) return parsed.error();
    return std::make_pair(parsed.value(), port);
}

}  // namespace

UpstreamClient::UpstreamClient(UpstreamConfig config)
    : config_(std::move(config)) {}

void UpstreamClient::set_config(UpstreamConfig config) {
    config_ = std::move(config);
}

bool UpstreamClient::is_endpoint_excluded(const IpAddress& ip, std::uint16_t port) const {
    for (const auto& raw : config_.excluded_endpoints) {
        auto parsed = parse_endpoint(raw);
        if (!parsed) continue;
        if (parsed.value().first == ip && parsed.value().second == port) {
            return true;
        }
    }
    return false;
}


std::vector<UpstreamClient::Endpoint> UpstreamClient::parse_endpoints(
    const std::vector<std::string>& list) const {
    std::vector<Endpoint> out;
    for (const auto& item : list) {
        if (item == "auto") continue;
        auto parsed = parse_endpoint(item);
        if (!parsed) continue;
        if (is_endpoint_excluded(parsed.value().first, parsed.value().second)) continue;
        out.push_back(Endpoint{parsed.value().first, parsed.value().second, item});
    }
    return out;
}

Result<UpstreamResponse> UpstreamClient::resolve(const dns::Message& query,
                                                 UpstreamTraceCallback trace_cb) {
    auto primary = parse_endpoints(config_.servers);
    for (const auto& ep : primary) {
        auto res = try_server(ep, query, trace_cb);
        if (res) return res;
    }

    auto fallback = parse_endpoints(config_.fallback);
    for (const auto& ep : fallback) {
        auto res = try_server(ep, query, trace_cb);
        if (res) return res;
    }

    return Error{801, "all upstream servers failed"};
}

Result<UpstreamResponse> UpstreamClient::try_server(const Endpoint& target,
                                                    const dns::Message& query,
                                                    UpstreamTraceCallback trace_cb) {
    auto encoded = query.encode(512);
    if (!encoded) return encoded.error();

    const auto timeout = std::chrono::milliseconds(config_.timeout_ms);
    const auto start = std::chrono::steady_clock::now();

    for (std::uint32_t attempt = 0; attempt <= config_.retries; ++attempt) {
        auto udp_res = query_udp(target, encoded.value(), timeout);
        if (udp_res) {
            auto msg = std::move(udp_res.value());
            if (msg.id != query.id) continue;  // Drop mismatched / unsolicited response
            // If truncated, retry via TCP per RFC 1035 §4.2.2 / SPEC §6.3.
            if (msg.truncated()) {
                auto tcp_res = query_tcp(target, encoded.value(), timeout);
                if (tcp_res) {
                    const auto end = std::chrono::steady_clock::now();
                    const auto latency = std::chrono::duration_cast<std::chrono::microseconds>(
                        end - start).count();
                    if (trace_cb) {
                        trace_cb(UpstreamTrace{target.raw, true, false, true,
                                               static_cast<std::uint64_t>(latency), ""});
                    }
                    return UpstreamResponse{std::move(tcp_res.value()), target.raw,
                                            static_cast<std::uint64_t>(latency), true};
                }
            }

            const auto end = std::chrono::steady_clock::now();
            const auto latency = std::chrono::duration_cast<std::chrono::microseconds>(
                end - start).count();
            if (trace_cb) {
                trace_cb(UpstreamTrace{target.raw, true, false, false,
                                       static_cast<std::uint64_t>(latency), ""});
            }
            return UpstreamResponse{std::move(msg), target.raw,
                                    static_cast<std::uint64_t>(latency), false};
        }
    }

    const auto end = std::chrono::steady_clock::now();
    const auto latency = std::chrono::duration_cast<std::chrono::microseconds>(
        end - start).count();
    if (trace_cb) {
        trace_cb(UpstreamTrace{target.raw, false, true, false,
                               static_cast<std::uint64_t>(latency), "timeout or network error"});
    }
    return Error{802, "server " + target.raw + " timed out"};
}

Result<dns::Message> UpstreamClient::query_udp(const Endpoint& target,
                                               std::span<const std::uint8_t> wire,
                                               std::chrono::milliseconds timeout) {
    asio::io_context io;
    udp::socket socket(io);
    asio::error_code ec;

    const udp::endpoint server_ep(to_asio(target.ip), target.port);
    socket.open(server_ep.protocol(), ec);
    if (ec) return Error{803, "cannot open udp socket: " + ec.message()};
    socket.connect(server_ep, ec);
    if (ec) return Error{803, "cannot connect udp socket to upstream: " + ec.message()};

    std::array<std::uint8_t, 4096> recv_buf{};
    std::size_t received_bytes = 0;
    bool timed_out = false;

    asio::steady_timer timer(io);
    timer.expires_after(timeout);
    timer.async_wait([&](const asio::error_code& timer_ec) {
        if (!timer_ec) {
            timed_out = true;
            asio::error_code close_ec;
            socket.close(close_ec);
        }
    });

    socket.async_send(asio::buffer(wire.data(), wire.size()),
                      [&](const asio::error_code& send_ec, std::size_t) {
                          if (!send_ec) {
                              socket.async_receive(
                                  asio::buffer(recv_buf),
                                  [&](const asio::error_code& recv_ec, std::size_t bytes) {
                                      if (!recv_ec) {
                                          received_bytes = bytes;
                                      }
                                      timer.cancel();
                                  });
                          } else {
                              timer.cancel();
                          }
                      });

    io.run();
    if (timed_out) return Error{802, "udp timeout"};
    if (received_bytes < dns::kHeaderSize) return Error{804, "received truncated response"};

    return dns::Message::parse(std::span<const std::uint8_t>(recv_buf.data(), received_bytes));
}

Result<dns::Message> UpstreamClient::query_tcp(const Endpoint& target,
                                               std::span<const std::uint8_t> wire,
                                               std::chrono::milliseconds timeout) {
    asio::io_context io;
    tcp::socket socket(io);
    asio::error_code ec;

    const tcp::endpoint server_ep(to_asio(target.ip), target.port);
    socket.open(server_ep.protocol(), ec);
    if (ec) return Error{805, "cannot open tcp socket: " + ec.message()};

    bool timed_out = false;
    asio::steady_timer timer(io);
    timer.expires_after(timeout);
    timer.async_wait([&](const asio::error_code& timer_ec) {
        if (!timer_ec) {
            timed_out = true;
            asio::error_code close_ec;
            socket.close(close_ec);
        }
    });

    std::array<std::uint8_t, 2> length_hdr{
        static_cast<std::uint8_t>(wire.size() >> 8),
        static_cast<std::uint8_t>(wire.size() & 0xFF),
    };
    std::vector<std::uint8_t> recv_buf;

    socket.async_connect(server_ep, [&](const asio::error_code& conn_ec) {
        if (conn_ec) {
            timer.cancel();
            return;
        }
        std::array<asio::const_buffer, 2> bufs{
            asio::buffer(length_hdr),
            asio::buffer(wire.data(), wire.size()),
        };
        asio::async_write(socket, bufs, [&](const asio::error_code& write_ec, std::size_t) {
            if (write_ec) {
                timer.cancel();
                return;
            }
            auto len_buf = std::make_shared<std::array<std::uint8_t, 2>>();
            asio::async_read(socket, asio::buffer(*len_buf),
                             [&, len_buf](const asio::error_code& read_hdr_ec, std::size_t) {
                                 if (read_hdr_ec) {
                                     timer.cancel();
                                     return;
                                 }
                                 const std::size_t resp_len =
                                     (static_cast<std::size_t>((*len_buf)[0]) << 8) |
                                     (*len_buf)[1];
                                 if (resp_len < dns::kHeaderSize || resp_len > 65535) {
                                     timer.cancel();
                                     return;
                                 }
                                 recv_buf.resize(resp_len);
                                 asio::async_read(
                                     socket, asio::buffer(recv_buf),
                                     [&](const asio::error_code&, std::size_t) {
                                         timer.cancel();
                                     });
                             });
        });
    });

    io.run();
    if (timed_out) return Error{802, "tcp timeout"};
    if (recv_buf.size() < dns::kHeaderSize) return Error{804, "received truncated response"};

    return dns::Message::parse(recv_buf);
}

}  // namespace ldns::resolver
