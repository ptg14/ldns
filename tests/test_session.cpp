#include "doctest.hpp"

#include <asio.hpp>
#include <atomic>
#include <iostream>
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

TEST_CASE("Session completes authenticated PSK handshake and exchanges encrypted frames") {
    asio::io_context io;

    (void)crypto::init();
    const auto net_h = crypto::net_hash("psk-test-net");

    // Generate shared PSK and derive root keys
    std::vector<std::uint8_t> psk_bytes = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
        0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
        0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20
    };
    auto root_keys = crypto::derive_root_keys(psk_bytes);

    SessionOptions opt1;
    opt1.my_node_id = generate_node_id();
    opt1.my_name = "node-secure-initiator";
    opt1.net_hash = net_h;
    opt1.sync_port = 5391;
    opt1.dns_port = 53;
    opt1.k_root = root_keys.root;
    opt1.k_auth = root_keys.auth;

    SessionOptions opt2;
    opt2.my_node_id = generate_node_id();
    opt2.my_name = "node-secure-responder";
    opt2.net_hash = net_h;
    opt2.sync_port = 5392;
    opt2.dns_port = 53;
    opt2.k_root = root_keys.root;
    opt2.k_auth = root_keys.auth;

    asio::ip::tcp::acceptor acceptor(io, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0));
    const auto listen_port = acceptor.local_endpoint().port();

    std::shared_ptr<Session> inbound_session;
    std::shared_ptr<Session> outbound_session;

    std::atomic<bool> node1_established{false};
    std::atomic<bool> node2_established{false};
    std::atomic<int> message_received_by_responder{0};

    acceptor.async_accept([&](const asio::error_code& ec, asio::ip::tcp::socket sock) {
        REQUIRE_FALSE(ec);
        inbound_session = Session::create_inbound(
            std::move(sock), opt2,
            [&](std::shared_ptr<Session>) {
                node2_established = true;
            },
            [](std::shared_ptr<Session>, std::string_view r) {
                std::cout << "inbound closed: " << r << "\n";
            },
            [&](std::shared_ptr<Session>, const ParsedMessage& msg) {
                if (msg.type == MessageType::Peers) {
                    message_received_by_responder++;
                }
            });
        inbound_session->start();
    });

    asio::ip::tcp::endpoint target(asio::ip::address_v4::loopback(), listen_port);
    outbound_session = Session::create_outbound(
        io, target, opt1,
        [&](std::shared_ptr<Session>) {
            node1_established = true;
        },
        [](std::shared_ptr<Session>, std::string_view r) {
            std::cout << "outbound closed: " << r << "\n";
        },
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

    CHECK(outbound_session->peer_name() == "node-secure-responder");
    CHECK(inbound_session->peer_name() == "node-secure-initiator");

    // Send encrypted PEERS frame from initiator to responder
    outbound_session->send_peers({{generate_node_id(), {}, 5391}});
    for (int i = 0; i < 20 && message_received_by_responder == 0; ++i) {
        io.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(message_received_by_responder > 0);

    outbound_session->close("test complete");
    inbound_session->close("test complete");
}

TEST_CASE("Session rejects handshake when PSK configuration mismatches") {
    asio::io_context io;

    (void)crypto::init();
    const auto net_h = crypto::net_hash("mismatch-net");

    std::vector<std::uint8_t> psk_bytes(32, 0x42);
    auto root_keys = crypto::derive_root_keys(psk_bytes);

    SessionOptions opt1; // With PSK
    opt1.my_node_id = generate_node_id();
    opt1.my_name = "node-has-psk";
    opt1.net_hash = net_h;
    opt1.k_root = root_keys.root;
    opt1.k_auth = root_keys.auth;

    SessionOptions opt2; // Without PSK
    opt2.my_node_id = generate_node_id();
    opt2.my_name = "node-no-psk";
    opt2.net_hash = net_h;

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

TEST_CASE("Session rejects handshake when PSK keys differ") {
    asio::io_context io;

    (void)crypto::init();
    const auto net_h = crypto::net_hash("diff-key-net");

    std::vector<std::uint8_t> psk_bytes_1(32, 0x11);
    std::vector<std::uint8_t> psk_bytes_2(32, 0x22);

    auto root_keys_1 = crypto::derive_root_keys(psk_bytes_1);
    auto root_keys_2 = crypto::derive_root_keys(psk_bytes_2);

    SessionOptions opt1;
    opt1.my_node_id = generate_node_id();
    opt1.my_name = "node-key1";
    opt1.net_hash = net_h;
    opt1.k_root = root_keys_1.root;
    opt1.k_auth = root_keys_1.auth;

    SessionOptions opt2;
    opt2.my_node_id = generate_node_id();
    opt2.my_name = "node-key2";
    opt2.net_hash = net_h;
    opt2.k_root = root_keys_2.root;
    opt2.k_auth = root_keys_2.auth;

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

TEST_CASE("Session disconnects peer exceeding 200 UPDATE ops per second") {
    asio::io_context io;

    (void)crypto::init();
    const auto net_h = crypto::net_hash("rate-limit-net");

    SessionOptions opt1;
    opt1.my_node_id = generate_node_id();
    opt1.my_name = "node-flooder";
    opt1.net_hash = net_h;

    SessionOptions opt2;
    opt2.my_node_id = generate_node_id();
    opt2.my_name = "node-receiver";
    opt2.net_hash = net_h;

    asio::ip::tcp::acceptor acceptor(io, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0));
    const auto listen_port = acceptor.local_endpoint().port();

    std::shared_ptr<Session> inbound_session;
    std::shared_ptr<Session> outbound_session;

    std::atomic<bool> node2_established{false};
    std::atomic<bool> node2_closed{false};

    acceptor.async_accept([&](const asio::error_code& ec, asio::ip::tcp::socket sock) {
        REQUIRE_FALSE(ec);
        inbound_session = Session::create_inbound(
            std::move(sock), opt2,
            [&](std::shared_ptr<Session>) {
                node2_established = true;
            },
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
        [](std::shared_ptr<Session>, std::string_view) {},
        [](std::shared_ptr<Session>, const ParsedMessage&) {});
    outbound_session->start();

    for (int i = 0; i < 50 && !node2_established; ++i) {
        io.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(node2_established);

    // Flood receiver with 205 update ops
    UpdateMessage flood;
    flood.owner = opt1.my_node_id;
    flood.epoch = 1;
    flood.seq = 1;
    for (int i = 0; i < 205; ++i) {
        Op op;
        op.op = "put";
        op.name = "flood" + std::to_string(i) + ".lan";
        op.type = 1;
        flood.ops.push_back(std::move(op));
    }

    auto frame = encode_update_frame(flood);
    REQUIRE(frame);
    outbound_session->send_frame(std::move(frame.value()));

    for (int i = 0; i < 50 && !node2_closed; ++i) {
        io.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    CHECK(node2_closed);
    CHECK(inbound_session->state() == SessionState::Closed);
}
