#include "ipc/ipc_client.hpp"
#include "ipc/ipc_channel.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace ldns::ipc {

IpcClient::IpcClient(std::string endpoint)
    : endpoint_(endpoint.empty() ? default_ipc_endpoint() : std::move(endpoint)) {}

bool IpcClient::is_available(int timeout_ms) const {
#ifdef _WIN32
    if (WaitNamedPipeA(endpoint_.c_str(), static_cast<DWORD>(timeout_ms))) {
        return true;
    }
    // Alternatively, try opening directly
    HANDLE pipe = CreateFileA(endpoint_.c_str(), GENERIC_READ | GENERIC_WRITE,
                              0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe);
        return true;
    }
    return false;
#else
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, endpoint_.c_str(), sizeof(addr.sun_path) - 1);

    // Set non-blocking for timeout check
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int res = ::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    bool connected = false;
    if (res == 0) {
        connected = true;
    } else {
        struct pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLOUT;
        if (::poll(&pfd, 1, timeout_ms) > 0 && (pfd.revents & POLLOUT)) {
            int err = 0;
            socklen_t len = sizeof(err);
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0) {
                connected = true;
            }
        }
    }

    ::close(fd);
    return connected;
#endif
}

Result<Response> IpcClient::call(std::string_view method,
                                 const nlohmann::json& params,
                                 int timeout_ms) {
    Request req;
    req.method = std::string(method);
    req.params = params;
    req.id = 1;

    std::string payload = serialize_request(req) + "\n";

#ifdef _WIN32
    if (!WaitNamedPipeA(endpoint_.c_str(), static_cast<DWORD>(timeout_ms))) {
        return Error{error_codes::kDaemonUnreachable, "Daemon is unreachable: " + endpoint_};
    }

    HANDLE pipe = CreateFileA(endpoint_.c_str(), GENERIC_READ | GENERIC_WRITE,
                              0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        return Error{error_codes::kDaemonUnreachable, "Cannot connect to named pipe: " + endpoint_};
    }

    DWORD bytes_written = 0;
    if (!WriteFile(pipe, payload.data(), static_cast<DWORD>(payload.size()), &bytes_written, nullptr)) {
        CloseHandle(pipe);
        return Error{error_codes::kGeneralError, "Failed to write to daemon pipe"};
    }
    FlushFileBuffers(pipe);

    std::string resp_buffer;
    char chunk[1024];
    DWORD bytes_read = 0;

    while (ReadFile(pipe, chunk, sizeof(chunk), &bytes_read, nullptr) && bytes_read > 0) {
        resp_buffer.append(chunk, bytes_read);
        if (resp_buffer.find('\n') != std::string::npos) {
            break;
        }
    }
    CloseHandle(pipe);

    auto line_end = resp_buffer.find('\n');
    std::string line = (line_end != std::string::npos) ? resp_buffer.substr(0, line_end) : resp_buffer;

    return parse_response(line);
#else
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return Error{error_codes::kGeneralError, "Failed to create socket"};
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, endpoint_.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return Error{error_codes::kDaemonUnreachable, "Daemon is unreachable: " + endpoint_};
    }

    (void)timeout_ms; // Using socket
    ssize_t written = ::write(fd, payload.data(), payload.size());
    if (written < 0) {
        ::close(fd);
        return Error{error_codes::kGeneralError, "Failed to write to daemon socket"};
    }

    std::string resp_buffer;
    char chunk[1024];
    ssize_t n = 0;

    while ((n = ::read(fd, chunk, sizeof(chunk))) > 0) {
        resp_buffer.append(chunk, static_cast<std::size_t>(n));
        if (resp_buffer.find('\n') != std::string::npos) {
            break;
        }
    }
    ::close(fd);

    auto line_end = resp_buffer.find('\n');
    std::string line = (line_end != std::string::npos) ? resp_buffer.substr(0, line_end) : resp_buffer;

    return parse_response(line);
#endif
}

} // namespace ldns::ipc
