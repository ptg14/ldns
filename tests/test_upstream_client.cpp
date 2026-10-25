#include "doctest.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <thread>
#include <asio.hpp>

#include "core/dns/message.hpp"
#include "core/dns/name.hpp"
#include "core/resolver/upstream_client.hpp"

using namespace ldns;
using namespace ldns::dns;
using namespace ldns::resolver;

namespace {

using asio::ip::udp;
using asio::ip::tcp;

class MockDnsServer {
public:
    explicit MockDnsServer(bool respond_tc = false)
        : io_(), udp_socket_(io_), tcp_acceptor_(io_), respond_tc_(respond_tc) {
        udp_socket_.open(udp::v4());
        udp_socket_.bind(udp::endpoint(asio::ip::make_address_v4("127.0.0.1"), 0));
        port_ = udp_socket_.local_endpoint().port();

        tcp_acceptor_.open(tcp::v4());
        tcp_acceptor_.bind(tcp::endpoint(asio::ip::make_address_v4("127.0.0.1"), port_));
        tcp_acceptor_.listen();

        start_udp();
        start_tcp();

        thread_ = std::thread([this] { io_.run(); });
    }

    ~MockDnsServer() {
        stop();
    }

    void stop() {
        if (!stopped_.exchange(true)) {
            asio::error_code ec;
            udp_socket_.close(ec);
            tcp_acceptor_.close(ec);
            io_.stop();
            if (thread_.joinable()) thread_.join();
        }
    }

    std::uint16_t port() const noexcept { return port_; }
    std::string endpoint_str() const { return "127.0.0.1:" + std::to_string(port_); }

private:
    void start_udp() {
        udp_socket_.async_receive_from(
            asio::buffer(udp_buf_), udp_sender_,
            [this](const asio::error_code& ec, std::size_t bytes) {
                if (ec || bytes < kHeaderSize) return;
                auto parsed = Message::parse(std::span<const std::uint8_t>(udp_buf_.data(), bytes));
                if (!parsed) return;

                Message response;
                response.id = parsed.value().id;
                response.flags = kQr | kRa;
                response.questions = parsed.value().questions;

                if (respond_tc_) {
                    response.set_flag(kTc, true);
                } else {
                    ResourceRecord rr;
                    rr.name = parsed.value().questions[0].name;
                    rr.type = 1;
                    rr.rr_class = 1;
                    rr.ttl = 300;
                    const std::uint8_t ip[4] = {1, 2, 3, 4};
                    rr.rdata = encode_rdata_a(ip).value();
                    response.answers.push_back(rr);
                }

                auto enc = response.encode(512);
                if (enc) {
                    auto enc_buf = std::make_shared<std::vector<std::uint8_t>>(std::move(enc.value()));
                    udp_socket_.async_send_to(
                        asio::buffer(*enc_buf), udp_sender_,
                        [this, enc_buf](const asio::error_code&, std::size_t) {
                            if (!stopped_) start_udp();
                        });
                }
            });
    }

    void start_tcp() {
        tcp_acceptor_.async_accept([this](const asio::error_code& ec, tcp::socket socket) {
            if (ec) return;
            auto sock = std::make_shared<tcp::socket>(std::move(socket));
            auto len_buf = std::make_shared<std::array<std::uint8_t, 2>>();
            asio::async_read(
                *sock, asio::buffer(*len_buf),
                [this, sock, len_buf](const asio::error_code& read_ec, std::size_t) {
                    if (read_ec) return;
                    std::size_t len = (static_cast<std::size_t>((*len_buf)[0]) << 8) | (*len_buf)[1];
                    auto buf = std::make_shared<std::vector<std::uint8_t>>(len);
                    asio::async_read(
                        *sock, asio::buffer(*buf),
                        [this, sock, buf](const asio::error_code& body_ec, std::size_t) {
                            if (body_ec) return;
                            auto parsed = Message::parse(*buf);
                            if (!parsed) return;

                            Message response;
                            response.id = parsed.value().id;
                            response.flags = kQr | kRa;
                            response.questions = parsed.value().questions;

                            ResourceRecord rr;
                            rr.name = parsed.value().questions[0].name;
                            rr.type = 1;
                            rr.rr_class = 1;
                            rr.ttl = 300;
                            const std::uint8_t ip[4] = {9, 8, 7, 6};
                            rr.rdata = encode_rdata_a(ip).value();
                            response.answers.push_back(rr);

                            auto enc = response.encode(65535);
                            if (enc) {
                                auto frame = std::make_shared<std::vector<std::uint8_t>>();
                                frame->push_back(static_cast<std::uint8_t>(enc.value().size() >> 8));
                                frame->push_back(static_cast<std::uint8_t>(enc.value().size() & 0xFF));
                                frame->insert(frame->end(), enc.value().begin(), enc.value().end());
                                asio::async_write(
                                    *sock, asio::buffer(*frame),
                                    [sock, frame](const asio::error_code&, std::size_t) {});
                            }
                        });
                });
            if (!stopped_) start_tcp();
        });
    }

    asio::io_context io_;
    udp::socket udp_socket_;
    tcp::acceptor tcp_acceptor_;
    std::uint16_t port_{0};
    bool respond_tc_{false};
    std::atomic<bool> stopped_{false};
    std::thread thread_;
    std::array<std::uint8_t, 4096> udp_buf_{};
    udp::endpoint udp_sender_;
};

}  // namespace

TEST_CASE("UpstreamClient resolves via UDP") {
    MockDnsServer server(false);

    UpstreamConfig config;
    config.servers = {server.endpoint_str()};
    config.timeout_ms = 1000;
    config.retries = 0;

    UpstreamClient client(config);

    Message query;
    query.id = 1234;
    query.questions.push_back(Question{Name::from_presentation("google.com").value(), 1, 1});

    auto res = client.resolve(query);
    REQUIRE(res.has_value());
    CHECK_FALSE(res.value().used_tcp);
    REQUIRE(res.value().message.answers.size() == 1);
    CHECK(res.value().message.answers[0].rdata.size() == 4);
    CHECK(res.value().message.answers[0].rdata[0] == 1);
    CHECK(res.value().message.answers[0].rdata[1] == 2);
    CHECK(res.value().message.answers[0].rdata[2] == 3);
    CHECK(res.value().message.answers[0].rdata[3] == 4);
}

TEST_CASE("UpstreamClient retries via TCP when TC=1 is received") {
    MockDnsServer server(true);  // UDP responds with TC=1

    UpstreamConfig config;
    config.servers = {server.endpoint_str()};
    config.timeout_ms = 1000;
    config.retries = 0;

    UpstreamClient client(config);

    Message query;
    query.id = 5678;
    query.questions.push_back(Question{Name::from_presentation("large.com").value(), 1, 1});

    auto res = client.resolve(query);
    REQUIRE(res.has_value());
    CHECK(res.value().used_tcp);
    REQUIRE(res.value().message.answers.size() == 1);
    CHECK(res.value().message.answers[0].rdata[0] == 9);
    CHECK(res.value().message.answers[0].rdata[1] == 8);
}

TEST_CASE("UpstreamClient loop prevention excludes matching endpoints") {
    MockDnsServer server(false);

    UpstreamConfig config;
    config.servers = {server.endpoint_str()};
    config.excluded_endpoints = {server.endpoint_str()};  // exclude our mock server!

    UpstreamClient client(config);

    Message query;
    query.id = 9999;
    query.questions.push_back(Question{Name::from_presentation("loop.com").value(), 1, 1});

    auto res = client.resolve(query);
    REQUIRE_FALSE(res.has_value());
    CHECK(res.error().code == 801);  // all upstreams failed because server was excluded
}
