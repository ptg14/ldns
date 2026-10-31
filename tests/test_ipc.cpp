#include "doctest.hpp"

#include "core/core_api.hpp"
#include "ipc/ipc_channel.hpp"
#include "ipc/ipc_client.hpp"
#include "ipc/ipc_server.hpp"
#include "ipc/json_rpc.hpp"

#include <chrono>
#include <filesystem>
#include <thread>

using namespace ldns;
using namespace ldns::ipc;

namespace {

struct TempDir {
    std::filesystem::path path;

    TempDir() {
        path = std::filesystem::temp_directory_path() /
               ("ldns-ipc-test-" + std::to_string(::rand()) + "-" +
                std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }

    std::string file(const std::string& name) const { return (path / name).string(); }
};

CoreApi::Options options_for(const TempDir& dir) {
    CoreApi::Options options;
    options.paths.config_file = dir.file("config.toml");
    options.paths.data_file = dir.file("ldns.db");
    return options;
}

std::string make_test_endpoint() {
    static std::atomic<int> counter{0};
    int id = ++counter;
#ifdef _WIN32
    return "\\\\.\\pipe\\ldns-test-pipe-" + std::to_string(::rand()) + "-" + std::to_string(id);
#else
    return "/tmp/ldns-test-sock-" + std::to_string(::rand()) + "-" + std::to_string(id) + ".sock";
#endif
}

} // namespace

TEST_CASE("JSON-RPC 2.0 Request Parsing & Serialization") {
    SUBCASE("Valid request with object params and integer id") {
        std::string raw = R"({"jsonrpc": "2.0", "method": "zone.add", "params": {"name": "app.internal"}, "id": 42})";
        auto res = parse_request(raw);
        REQUIRE(res.has_value());
        const auto& req = res.value();
        CHECK(req.jsonrpc == "2.0");
        CHECK(req.method == "zone.add");
        CHECK(req.params["name"] == "app.internal");
        CHECK(req.id == 42);
        CHECK_FALSE(req.is_notification());

        std::string serialized = serialize_request(req);
        auto re_parsed = parse_request(serialized);
        REQUIRE(re_parsed.has_value());
        CHECK(re_parsed.value().method == "zone.add");
        CHECK(re_parsed.value().id == 42);
    }

    SUBCASE("Valid notification (no id)") {
        std::string raw = R"({"jsonrpc": "2.0", "method": "notify.peer", "params": [1, 2, 3]})";
        auto res = parse_request(raw);
        REQUIRE(res.has_value());
        CHECK(res.value().is_notification());
        CHECK(res.value().id.is_null());
        CHECK(res.value().params.is_array());
    }

    SUBCASE("Invalid requests") {
        CHECK_FALSE(parse_request("not json").has_value());
        CHECK_FALSE(parse_request(R"({"method": "test"})").has_value()); // missing jsonrpc
        CHECK_FALSE(parse_request(R"({"jsonrpc": "1.0", "method": "test"})").has_value()); // wrong version
        CHECK_FALSE(parse_request(R"({"jsonrpc": "2.0"})").has_value()); // missing method
        CHECK_FALSE(parse_request(R"({"jsonrpc": "2.0", "method": 123})").has_value()); // method not string
    }
}

TEST_CASE("JSON-RPC 2.0 Response Handling") {
    SUBCASE("Success response") {
        auto resp = Response::make_success(101, nlohmann::json{{"status", "ok"}});
        CHECK(resp.is_success());
        CHECK(resp.id == 101);
        CHECK(resp.result.value()["status"] == "ok");

        std::string serialized = serialize_response(resp);
        auto parsed = parse_response(serialized);
        REQUIRE(parsed.has_value());
        CHECK(parsed.value().is_success());
        CHECK(parsed.value().id == 101);
        CHECK(parsed.value().result.value()["status"] == "ok");
    }

    SUBCASE("Error response") {
        auto resp = Response::make_error(102, error_codes::kMethodNotFound, "Method not found");
        CHECK_FALSE(resp.is_success());
        CHECK(resp.id == 102);
        REQUIRE(resp.error.has_value());
        CHECK(resp.error->code == error_codes::kMethodNotFound);
        CHECK(resp.error->message == "Method not found");

        std::string serialized = serialize_response(resp);
        auto parsed = parse_response(serialized);
        REQUIRE(parsed.has_value());
        CHECK_FALSE(parsed.value().is_success());
        CHECK(parsed.value().error->code == error_codes::kMethodNotFound);
    }
}

TEST_CASE("IPC Server & Client End-to-End") {
    TempDir temp;
    auto api_res = CoreApi::create(options_for(temp));
    REQUIRE(api_res.has_value());
    auto& api = *api_res.value();

    std::string endpoint = make_test_endpoint();
    IpcServer server(api, endpoint);

    std::atomic<bool> stopped_flag{false};
    server.set_shutdown_handler([&stopped_flag]() {
        stopped_flag = true;
    });

    auto start_res = server.start();
    REQUIRE(start_res.has_value());
    CHECK(server.is_running());

    // Allow server thread to initialize
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    IpcClient client(endpoint);
    CHECK(client.is_available(2000));

    SUBCASE("call status") {
        auto res = client.call("status");
        REQUIRE(res.has_value());
        const auto& resp = res.value();
        REQUIRE(resp.is_success());
        REQUIRE(resp.result.has_value());
        CHECK(resp.result.value()["running"] == true);
        CHECK(resp.result.value()["version"] == "0.1.0");
    }

    SUBCASE("zone add and ls and resolve") {
        nlohmann::json add_params = {
            {"name", "mydev.internal"},
            {"type", "A"},
            {"targets", {"192.168.1.150"}},
            {"ttl", 300}
        };
        auto add_res = client.call("zone.add", add_params);
        REQUIRE(add_res.has_value());
        CHECK(add_res.value().is_success());

        auto ls_res = client.call("zone.ls");
        REQUIRE(ls_res.has_value());
        REQUIRE(ls_res.value().is_success());
        auto records = ls_res.value().result.value();
        CHECK(records.is_array());
        CHECK(records.size() == 1);
        CHECK(records[0]["name"] == "mydev.internal");

        nlohmann::json resolve_params = {
            {"name", "mydev.internal"},
            {"type", "A"}
        };
        auto resolve_res = client.call("resolve", resolve_params);
        REQUIRE(resolve_res.has_value());
        REQUIRE(resolve_res.value().is_success());
        auto answers = resolve_res.value().result.value()["answers"];
        REQUIRE(answers.is_array());
        REQUIRE(answers.size() == 1);
        CHECK(answers[0]["rdata"] == "192.168.1.150");
    }

    SUBCASE("unknown method returns MethodNotFound") {
        auto res = client.call("nonexistent_method");
        REQUIRE(res.has_value());
        const auto& resp = res.value();
        CHECK_FALSE(resp.is_success());
        REQUIRE(resp.error.has_value());
        CHECK(resp.error->code == error_codes::kMethodNotFound);
    }

    SUBCASE("daemon.stop triggers shutdown handler") {
        auto res = client.call("daemon.stop");
        REQUIRE(res.has_value());
        CHECK(res.value().is_success());
        CHECK(stopped_flag == true);
    }

    server.stop();
    CHECK_FALSE(server.is_running());
}
