#pragma once

#include <chrono>
#include <string>

#include "core/util/result.hpp"
#include "ipc/json_rpc.hpp"

namespace ldns::ipc {

class IpcClient {
public:
    explicit IpcClient(std::string endpoint = "");

    /// Checks if a daemon server is reachable on the endpoint.
    bool is_available(int timeout_ms = 500) const;

    /// Sends a JSON-RPC request to the daemon and waits for the response.
    Result<Response> call(std::string_view method,
                          const nlohmann::json& params = nlohmann::json::object(),
                          int timeout_ms = 3000);

    const std::string& endpoint() const { return endpoint_; }

private:
    std::string endpoint_;
};

} // namespace ldns::ipc
