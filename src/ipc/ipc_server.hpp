#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/core_api.hpp"
#include "ipc/json_rpc.hpp"

namespace ldns::ipc {

class IpcServer {
public:
    using ShutdownHandler = std::function<void()>;

    IpcServer(CoreApi& api, std::string endpoint = "");
    ~IpcServer();

    IpcServer(const IpcServer&) = delete;
    IpcServer& operator=(const IpcServer&) = delete;

    /// Starts the IPC server listener on a background thread.
    Result<void> start();

    /// Stops the IPC server listener and closes the endpoint.
    void stop();

    /// Checks if the server is running.
    bool is_running() const { return running_; }

    /// The endpoint string (named pipe path or unix domain socket path).
    const std::string& endpoint() const { return endpoint_; }

    /// Sets callback when client sends daemon.stop request.
    void set_shutdown_handler(ShutdownHandler handler) {
        shutdown_handler_ = std::move(handler);
    }

    /// Handles a single JSON-RPC request and produces a JSON-RPC response.
    Response handle_rpc_call(const Request& req);

private:
    void run_listener_thread();

    CoreApi& api_;
    std::string endpoint_;
    std::atomic<bool> running_{false};
    std::thread listener_thread_;
    ShutdownHandler shutdown_handler_;

#ifdef _WIN32
    void* pipe_handle_{nullptr};
#else
    int server_fd_{-1};
#endif
};

} // namespace ldns::ipc
