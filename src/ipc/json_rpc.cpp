#include "ipc/json_rpc.hpp"

namespace ldns::ipc {

Response Response::make_success(nlohmann::json id, nlohmann::json result) {
    Response r;
    r.id = std::move(id);
    r.result = std::move(result);
    return r;
}

Response Response::make_error(nlohmann::json id, int code, std::string message,
                              nlohmann::json data) {
    Response r;
    r.id = std::move(id);
    RpcError err;
    err.code = code;
    err.message = std::move(message);
    err.data = std::move(data);
    r.error = std::move(err);
    return r;
}

Result<Request> parse_request(std::string_view json_str) {
    try {
        auto j = nlohmann::json::parse(json_str);
        if (!j.is_object()) {
            return Error{error_codes::kInvalidRequest, "JSON-RPC request must be a JSON object"};
        }

        Request req;
        if (j.contains("jsonrpc") && j["jsonrpc"].is_string() && j["jsonrpc"] == "2.0") {
            req.jsonrpc = "2.0";
        } else {
            return Error{error_codes::kInvalidRequest, "Missing or invalid 'jsonrpc' field (must be '2.0')"};
        }

        if (j.contains("method") && j["method"].is_string()) {
            req.method = j["method"].get<std::string>();
        } else {
            return Error{error_codes::kInvalidRequest, "Missing or invalid 'method' field"};
        }

        if (j.contains("params")) {
            req.params = j["params"];
        }

        if (j.contains("id")) {
            req.id = j["id"];
        }

        return req;
    } catch (const nlohmann::json::parse_error& e) {
        return Error{error_codes::kParseError, std::string("JSON parse error: ") + e.what()};
    } catch (const std::exception& e) {
        return Error{error_codes::kInvalidRequest, std::string("Invalid request: ") + e.what()};
    }
}

Result<Response> parse_response(std::string_view json_str) {
    try {
        auto j = nlohmann::json::parse(json_str);
        if (!j.is_object()) {
            return Error{error_codes::kInvalidRequest, "JSON-RPC response must be a JSON object"};
        }

        Response resp;
        if (j.contains("jsonrpc") && j["jsonrpc"].is_string()) {
            resp.jsonrpc = j["jsonrpc"].get<std::string>();
        }

        if (j.contains("id")) {
            resp.id = j["id"];
        }

        if (j.contains("result")) {
            resp.result = j["result"];
        }

        if (j.contains("error") && j["error"].is_object()) {
            const auto& err_obj = j["error"];
            RpcError err;
            if (err_obj.contains("code") && err_obj["code"].is_number()) {
                err.code = err_obj["code"].get<int>();
            }
            if (err_obj.contains("message") && err_obj["message"].is_string()) {
                err.message = err_obj["message"].get<std::string>();
            }
            if (err_obj.contains("data")) {
                err.data = err_obj["data"];
            }
            resp.error = std::move(err);
        }

        return resp;
    } catch (const nlohmann::json::parse_error& e) {
        return Error{error_codes::kParseError, std::string("JSON parse error: ") + e.what()};
    } catch (const std::exception& e) {
        return Error{error_codes::kInvalidRequest, std::string("Invalid response: ") + e.what()};
    }
}

std::string serialize_request(const Request& req) {
    nlohmann::json j;
    j["jsonrpc"] = req.jsonrpc;
    j["method"] = req.method;
    if (!req.params.is_null()) {
        j["params"] = req.params;
    }
    if (!req.id.is_null()) {
        j["id"] = req.id;
    }
    return j.dump();
}

std::string serialize_response(const Response& res) {
    nlohmann::json j;
    j["jsonrpc"] = res.jsonrpc;
    j["id"] = res.id;
    if (res.error.has_value()) {
        nlohmann::json err_obj;
        err_obj["code"] = res.error->code;
        err_obj["message"] = res.error->message;
        if (!res.error->data.is_null()) {
            err_obj["data"] = res.error->data;
        }
        j["error"] = err_obj;
    } else if (res.result.has_value()) {
        j["result"] = *res.result;
    } else {
        j["result"] = nullptr;
    }
    return j.dump();
}

} // namespace ldns::ipc
