#include "doctest.hpp"

#include "core/server/dns_server.hpp"

#include <asio.hpp>

#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <vector>

using namespace ldns;
using namespace ldns::dns;

namespace {

Name nm(const std::string& text) { return Name::from_presentation(text).value(); }

std::vector<std::uint8_t> make_query(const std::string& name, std::uint16_t id = 0x4242) {
    Message msg;
    msg.id = id;
    msg.flags = kRd;
    Question q;
    q.name = nm(name);
    q.type = 1;
    msg.questions.push_back(q);
    auto encoded = msg.encode();
    REQUIRE(encoded.has_value());
    return encoded.value();
}

// Minimal TCP DNS client: length prefix, one query, one response. The socket
// is non-blocking with a polling loop so a hang cannot wedge the test run.
std::vector<std::uint8_t> tcp_query(const std::string& host, std::uint16_t port,
                                    const std::vector<std::uint8_t>& packet) {
    asio::io_context io;
    asio::ip::tcp::socket socket(io);
    asio::ip::tcp::endpoint endpoint(asio::ip::make_address(host), port);
    socket.connect(endpoint);

    std::vector<std::uint8_t> frame;
    frame.push_back(static_cast<std::uint8_t>(packet.size() >> 8));
    frame.push_back(static_cast<std::uint8_t>(packet.size() & 0xFF));
    frame.insert(frame.end(), packet.begin(), packet.end());
    asio::write(socket, asio::buffer(frame));

    std::array<std::uint8_t, 2> length{};
    asio::read(socket, asio::buffer(length));
    const std::size_t size = (static_cast<std::size_t>(length[0]) << 8) | length[1];
    std::vector<std::uint8_t> response(size);
    asio::read(socket, asio::buffer(response));
    return response;
}

// Joins an io_context thread on scope exit, including when an assertion
// throws, so a failed CHECK cannot turn into std::terminate.
class IoRunner {
public:
    explicit IoRunner(asio::io_context& io) : thread_([&io] {
        try {
            io.run();
        } catch (...) {
            // Exceptions escaping a handler must not kill the process.
        }
    }) {}

    ~IoRunner() {
        if (thread_.joinable()) thread_.join();
    }

    IoRunner(const IoRunner&) = delete;
    IoRunner& operator=(const IoRunner&) = delete;

private:
    std::thread thread_;
};

}  // namespace

TEST_CASE("UDP and TCP serve the local zone on a loopback port") {
    asio::io_context io;
    LocalZone zone;
    const auto a = parse_ipv4("192.168.1.10").value();
    REQUIRE(zone.upsert(nm("app.lan"), static_cast<std::uint16_t>(RrType::A), 60, {a})
                .has_value());

    const QueryHandler handler(zone);
    ServerConfig config;
    config.listen = {"127.0.0.1:0"};
    DnsServer server(io, handler, config);
    auto status = server.start();
    REQUIRE(status.has_value());

    // The runner must outlive every assertion below so io_context handles keep
    // running and the thread is always joined.
    IoRunner runner(io);

    const auto bound = server.bound_addresses();
    REQUIRE(bound.size() == 1);
    const std::string text = bound.front();
    const std::uint16_t port =
        static_cast<std::uint16_t>(std::stoi(text.substr(text.rfind(':') + 1)));

    // UDP
    {
        asio::io_context client_io;
        asio::ip::udp::socket socket(client_io);
        socket.open(asio::ip::udp::v4());
        const auto query = make_query("app.lan");
        asio::ip::udp::endpoint server_endpoint(asio::ip::make_address("127.0.0.1"), port);
        socket.send_to(asio::buffer(query), server_endpoint);

        std::array<std::uint8_t, 2048> buffer{};
        asio::ip::udp::endpoint from;
        socket.non_blocking(true);
        std::size_t received = 0;
        for (int i = 0; i < 200 && received == 0; ++i) {
            asio::error_code ec;
            received = socket.receive_from(asio::buffer(buffer), from, 0, ec);
            if (ec) {
                received = 0;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        REQUIRE(received > 0);

        auto parsed = Message::parse(std::span<const std::uint8_t>(buffer.data(), received));
        REQUIRE(parsed.has_value());
        CHECK(parsed.value().id == 0x4242);
        CHECK(parsed.value().is_response());
        REQUIRE(parsed.value().answers.size() == 1);
        CHECK(format_ipv4(parsed.value().answers[0].rdata) == "192.168.1.10");
    }

    // TCP
    {
        const auto response = tcp_query("127.0.0.1", port, make_query("app.lan"));
        auto parsed = Message::parse(response);
        REQUIRE(parsed.has_value());
        CHECK(parsed.value().is_response());
        REQUIRE(parsed.value().answers.size() == 1);
        CHECK(format_ipv4(parsed.value().answers[0].rdata) == "192.168.1.10");
    }

    server.stop();
    io.stop();
}

TEST_CASE("server refuses wildcard bind addresses") {
    asio::io_context io;
    LocalZone zone;
    const QueryHandler handler(zone);
    ServerConfig config;
    config.listen = {"0.0.0.0:5300"};
    DnsServer server(io, handler, config);
    const auto status = server.start();
    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().code == 317);
}

TEST_CASE("server reports a bind conflict") {
    asio::io_context io;
    LocalZone zone;
    const QueryHandler handler(zone);

    ServerConfig first_config;
    first_config.listen = {"127.0.0.1:0"};
    DnsServer first(io, handler, first_config);
    REQUIRE(first.start().has_value());
    const std::string bound = first.bound_addresses().front();
    const std::uint16_t port =
        static_cast<std::uint16_t>(std::stoi(bound.substr(bound.rfind(':') + 1)));

    ServerConfig second_config;
    second_config.listen = {"127.0.0.1:" + std::to_string(port)};
    DnsServer second(io, handler, second_config);
    const auto status = second.start();
    REQUIRE_FALSE(status.has_value());
    CHECK(status.error().code == 312);

    first.stop();
}
