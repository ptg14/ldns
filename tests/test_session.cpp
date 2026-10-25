#include "doctest.hpp"

#include <asio.hpp>
#include <atomic>
#include <thread>

#include "core/net/crypto.hpp"
#include "core/net/session.hpp"

using namespace ldns;
using namespace ldns::net;

TEST_CASE("Session completes handshake and transitions to established") {
    asio::io_context io;

    (void)crypto::init();
    const auto net_h = crypto::net_hash("session-test-net");

    SessionOptions opt1;
    opt1.my_node_id = generate_node_id();
    opt1.my_name = "node-alpha";
    opt1.net_hash = net_h;
    opt1.sync_port = 5391;
    opt1.dns_port = 53;

    SessionOptions opt2;
    opt2.my_node_id = generate_node_id();
    opt2.my_name = "node-beta";
    opt2.net_hash = net_h;
    opt2.sync_port = 5392;
    opt2.dns_port = 53;

    // Start TCP acceptor for Node 2
    asio::ip::tcp::acceptor acceptor(io, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0));
    const auto listen_port = acceptor.local_endpoint().port();

    std::shared_ptr<Session> inbound_session;
    std::shared_ptr<Session> outbound_session;

    std::atomic<bool> node1_established{false};
    std::atomic<bool> node2_established{false};

    acceptor.async_accept([&](const asio::error_code& ec, asio::ip::tcp::socket sock) {
        REQUIRE_FALSE(ec);
        inbound_session = Session::create_inbound(
            std::move(sock), opt2,
            [&](std::shared_ptr<Session>) {
                node2_established = true;
            },
            [](std::shared_ptr<Session>, std::string_view) {},
            [](std::shared_ptr<Session>, const ParsedMessage&) {});
        inbound_session->start();
    });

    asio::ip::tcp::endpoint target(asio::ip::address_v4::loopback(), listen_port);
    outbound_session = Session::create_outbound(
        io, target, opt1,
        [&](std::shared_ptr<Session>) {
            node1_established = true;
        },
        [](std::shared_ptr<Session>, std::string_view) {},
        [](std::shared_ptr<Session>, const ParsedMessage&) {});
    outbound_session->start();

    for (int i = 0; i < 50 && (!node1_established || !node2_established); ++i) {
        io.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    CHECK(node1_established);
    CHECK(node2_established);
    CHECK(outbound_session->state() == SessionState::Established);
    CHECK(inbound_session->state() == SessionState::Established);

    CHECK(outbound_session->peer_name() == "node-beta");
    CHECK(inbound_session->peer_name() == "node-alpha");

    outbound_session->close("test complete");
    inbound_session->close("test complete");
}

TEST_CASE("Session rejects connection with network mismatch") {
    asio::io_context io;

    (void)crypto::init();

    SessionOptions opt1;
    opt1.my_node_id = generate_node_id();
    opt1.my_name = "node-one";
    opt1.net_hash = crypto::net_hash("network-one");

    SessionOptions opt2;
    opt2.my_node_id = generate_node_id();
    opt2.my_name = "node-two";
    opt2.net_hash = crypto::net_hash("network-two"); // different!

    asio::ip::tcp::acceptor acceptor(io, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0));
    const auto listen_port = acceptor.local_endpoint().port();

    std::shared_ptr<Session> inbound_session;
    std::shared_ptr<Session> outbound_session;

    std::atomic<bool> node1_closed{false};
    std::atomic<bool> node2_closed{false};

    acceptor.async_accept([&](const asio::error_code& ec, asio::ip::tcp::socket sock) {
        REQUIRE_FALSE(ec);
        inbound_session = Session::create_inbound(
            std::move(sock), opt2,
            [](std::shared_ptr<Session>) {},
            [&](std::shared_ptr<Session>, std::string_view) {
                node2_closed = true;
            },
            [](std::shared_ptr<Session>, const ParsedMessage&) {});
        inbound_session->start();
    });

    asio::ip::tcp::endpoint target(asio::ip::address_v4::loopback(), listen_port);
    outbound_session = Session::create_outbound(
        io, target, opt1,
        [](std::shared_ptr<Session>) {},
        [&](std::shared_ptr<Session>, std::string_view) {
            node1_closed = true;
        },
        [](std::shared_ptr<Session>, const ParsedMessage&) {});
    outbound_session->start();

    for (int i = 0; i < 50 && (!node1_closed || !node2_closed); ++i) {
        io.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    CHECK(node2_closed);
    CHECK(inbound_session->state() == SessionState::Closed);
}
