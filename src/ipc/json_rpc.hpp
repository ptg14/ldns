#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>

#include "core/util/result.hpp"

namespace ldns::ipc {

namespace error_codes {
    inline constexpr int kParseError = -32700;
    inline constexpr int kInvalidRequest = -32600;
    inline constexpr int kMethodNotFound = -32601;
    inline constexpr int kInvalidParams = -32602;
    inline constexpr int kInternalError = -32603;

    // Ldns domain-specific exit codes (SPEC §10.1)
    inline constexpr int kGeneralError = 1;
    inline constexpr int kUsageError = 2;
    inline constexpr int kDaemonUnreachable = 3;
    inline constexpr int kPrivilegeError = 4;
    inline constexpr int kConflictError = 5;
} // namespace error_codes

struct RpcError {
    int code{0};
    std::string message;
    nlohmann::json data = nullptr;
};

struct Request {
    std::string jsonrpc{"2.0"};
    std::string method;
    nlohmann::json params = nlohmann::json::object();
    nlohmann::json id = nullptr;

    bool is_notification() const { return id.is_null(); }
};

struct Response {
    std::string jsonrpc{"2.0"};
    nlohmann::json id = nullptr;
    std::optional<nlohmann::json> result{std::nullopt};
    std::optional<RpcError> error{std::nullopt};

    bool is_success() const { return !error.has_value(); }

    static Response make_success(nlohmann::json id, nlohmann::json result);
    static Response make_error(nlohmann::json id, int code, std::string message,
                               nlohmann::json data = nullptr);
};

/// Parses a JSON-RPC 2.0 Request from string.
Result<Request> parse_request(std::string_view json_str);

/// Parses a JSON-RPC 2.0 Response from string.
Result<Response> parse_response(std::string_view json_str);

/// Serializes Request to JSON string.
std::string serialize_request(const Request& req);

/// Serializes Response to JSON string.
std::string serialize_response(const Response& res);

} // namespace ldns::ipc
