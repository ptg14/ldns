#include "ipc/ipc_server.hpp"
#include "core/dns/message.hpp"
#include "core/util/id.hpp"
#include "core/util/logger.hpp"
#include "ipc/ipc_channel.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <sddl.h>
#ifndef PIPE_REJECT_REMOTE_CLIENTS
#define PIPE_REJECT_REMOTE_CLIENTS 0x00000008
#endif
#else
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace ldns::ipc {

namespace {

constexpr std::size_t kMaxIpcRequestBytes = 1024 * 1024; // 1 MiB

}  // namespace

IpcServer::IpcServer(CoreApi& api, std::string endpoint)
    : api_(api),
      endpoint_(endpoint.empty() ? default_ipc_endpoint() : std::move(endpoint)) {}

IpcServer::~IpcServer() {
    stop();
}

Result<void> IpcServer::start() {
    if (running_) return {};

    running_ = true;
    listener_thread_ = std::thread(&IpcServer::run_listener_thread, this);
    return {};
}

void IpcServer::stop() {
    if (!running_) return;

    running_ = false;

#ifdef _WIN32
    // Connect a dummy client to unblock ConnectNamedPipe
    HANDLE dummy = CreateFileA(endpoint_.c_str(), GENERIC_READ | GENERIC_WRITE,
                               0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (dummy != INVALID_HANDLE_VALUE) {
        CloseHandle(dummy);
    }
#else
    if (server_fd_ != -1) {
        ::unlink(endpoint_.c_str());
    }
#endif

    if (listener_thread_.joinable()) {
        listener_thread_.join();
    }
}

Response IpcServer::handle_rpc_call(const Request& req) {
    const std::string& method = req.method;
    const auto& params = req.params;

    if (method == "daemon.stop") {
        if (shutdown_handler_) {
            shutdown_handler_();
        }
        return Response::make_success(req.id, {{"status", "shutting_down"}});
    }

    if (method == "status") {
        auto st = api_.status();
        nlohmann::json j;
        j["running"] = true;
        j["version"] = LDNS_VERSION;
        j["node_id"] = to_hex(st.node_id);
        j["short_id"] = st.short_id;
        j["epoch"] = st.epoch;
        j["seq"] = st.seq;
        j["network_name"] = st.network_name;
        j["domain_count"] = st.domain_count;
        j["self_addresses"] = st.self_addresses;
        j["psk_configured"] = st.psk_configured;
        return Response::make_success(req.id, j);
    }

    if (method == "zone.add") {
        if (!params.contains("name") || !params["name"].is_string()) {
            return Response::make_error(req.id, error_codes::kInvalidParams, "Missing 'name' string");
        }
        std::string name = params["name"].get<std::string>();
        std::string type = params.value("type", "A");
        std::uint32_t ttl = params.value("ttl", 0u);
        bool is_self = params.value("is_self", false);

        if (is_self || !params.contains("targets")) {
            auto add_res = api_.add_self_domain(name, type, ttl);
            if (!add_res) {
                return Response::make_error(req.id, error_codes::kGeneralError, add_res.error().message);
            }
            nlohmann::json res_obj;
            res_obj["name"] = add_res.value().name;
            res_obj["type"] = add_res.value().type;
            res_obj["targets"] = add_res.value().targets;
            res_obj["ttl"] = add_res.value().ttl;
            res_obj["target_kind"] = add_res.value().target_kind;
            return Response::make_success(req.id, res_obj);
        } else {
            domain::AddRequest r;
            r.name = name;
            r.type = type;
            r.ttl = ttl;
            r.force = params.value("force", true);
            if (params["targets"].is_array()) {
                r.targets = params["targets"].get<std::vector<std::string>>();
            }
            auto add_res = api_.add_domain(std::move(r));
            if (!add_res) {
                return Response::make_error(req.id, error_codes::kGeneralError, add_res.error().message);
            }
            nlohmann::json res_obj;
            res_obj["name"] = add_res.value().name;
            res_obj["type"] = add_res.value().type;
            res_obj["targets"] = add_res.value().targets;
            res_obj["ttl"] = add_res.value().ttl;
            res_obj["target_kind"] = add_res.value().target_kind;
            return Response::make_success(req.id, res_obj);
        }
    }

    if (method == "zone.set") {
        if (!params.contains("name") || !params.contains("targets")) {
            return Response::make_error(req.id, error_codes::kInvalidParams, "Missing 'name' or 'targets'");
        }
        domain::AddRequest r;
        r.name = params["name"].get<std::string>();
        r.type = params.value("type", "A");
        if (params["targets"].is_array()) {
            r.targets = params["targets"].get<std::vector<std::string>>();
        }
        auto set_res = api_.set_domain(std::move(r));
        if (!set_res) {
            return Response::make_error(req.id, error_codes::kGeneralError, set_res.error().message);
        }
        return Response::make_success(req.id, {{"status", "updated"}});
    }

    if (method == "zone.rm") {
        if (!params.contains("name")) {
            return Response::make_error(req.id, error_codes::kInvalidParams, "Missing 'name'");
        }
        std::string name = params["name"].get<std::string>();
        std::string type = params.value("type", "A");
        auto rm_res = api_.remove_domain(name, type);
        if (!rm_res) {
            return Response::make_error(req.id, error_codes::kGeneralError, rm_res.error().message);
        }
        return Response::make_success(req.id, {{"status", "deleted"}});
    }

    if (method == "zone.ls") {
        std::optional<std::string> filter_type;
        if (params.contains("type") && params["type"].is_string()) {
            filter_type = params["type"].get<std::string>();
        }
        auto ls_res = api_.list_domains(filter_type);
        if (!ls_res) {
            return Response::make_error(req.id, error_codes::kGeneralError, ls_res.error().message);
        }
        nlohmann::json list_arr = nlohmann::json::array();
        for (const auto& d : ls_res.value()) {
            nlohmann::json item;
            item["name"] = d.name;
            item["type"] = d.type;
            item["ttl"] = d.ttl;
            item["target_kind"] = d.target_kind;
            item["targets"] = d.targets;
            list_arr.push_back(std::move(item));
        }
        return Response::make_success(req.id, list_arr);
    }

    if (method == "resolve") {
        if (!params.contains("name")) {
            return Response::make_error(req.id, error_codes::kInvalidParams, "Missing 'name'");
        }
        std::string name = params["name"].get<std::string>();
        std::string type = params.value("type", "A");
        bool trace = params.value("trace", false);

        auto res = api_.resolve(name, type, trace);
        if (!res) {
            return Response::make_error(req.id, error_codes::kGeneralError, res.error().message);
        }
        const auto& pr = res.value();
        nlohmann::json out;
        out["step"] = std::string(1, static_cast<char>(pr.resolved_step));
        out["total_latency_us"] = pr.total_latency_us;
        out["from_cache"] = pr.from_cache;
        nlohmann::json answers = nlohmann::json::array();
        for (const auto& a : pr.response.answers) {
            nlohmann::json ans;
            ans["name"] = a.name.to_string();
            ans["type"] = a.type;
            ans["ttl"] = a.ttl;
            ans["rdata"] = dns::format_rdata(a.type, a.rdata);
            answers.push_back(std::move(ans));
        }
        out["answers"] = std::move(answers);
        return Response::make_success(req.id, out);
    }

    if (method == "conflicts") {
        auto confs = api_.conflicts();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& c : confs) {
            nlohmann::json it;
            it["name"] = c.name.to_string();
            it["type"] = c.type;
            it["distinct_groups"] = c.groups.size();
            arr.push_back(std::move(it));
        }
        return Response::make_success(req.id, arr);
    }

    if (method == "peers.list") {
        auto peers_res = api_.peers();
        if (!peers_res) {
            return Response::make_error(req.id, error_codes::kGeneralError, peers_res.error().message);
        }
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& p : peers_res.value()) {
            nlohmann::json it;
            it["node_id"] = to_hex(p.node_id);
            it["name"] = p.name;
            it["last_addr"] = p.last_addr;
            arr.push_back(std::move(it));
        }
        return Response::make_success(req.id, arr);
    }

    return Response::make_error(req.id, error_codes::kMethodNotFound, "Method not found: " + method);
}

void IpcServer::run_listener_thread() {
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;
    PSECURITY_DESCRIPTOR p_sd = nullptr;
    // DACL: Allow Generic All (GA) to Builtin Administrators (BA), SYSTEM (SY), and Owner Rights (OW)
    if (ConvertStringSecurityDescriptorToSecurityDescriptorA(
            "D:(A;;GA;;;BA)(A;;GA;;;SY)(A;;GA;;;OW)", SDDL_REVISION_1, &p_sd, nullptr)) {
        sa.lpSecurityDescriptor = p_sd;
    }

    while (running_) {
        HANDLE pipe = CreateNamedPipeA(
            endpoint_.c_str(),
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            PIPE_UNLIMITED_INSTANCES,
            4096, 4096, 0, p_sd ? &sa : nullptr);

        if (pipe == INVALID_HANDLE_VALUE) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        BOOL connected = ConnectNamedPipe(pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (!running_) {
            CloseHandle(pipe);
            break;
        }

        if (connected) {
            std::string req_buffer;
            char chunk[1024];
            DWORD bytes_read = 0;

            DWORD total_waited_ms = 0;
            while (running_) {
                DWORD bytes_avail = 0;
                if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &bytes_avail, nullptr)) {
                    break;
                }
                if (bytes_avail > 0) {
                    DWORD to_read = (bytes_avail < sizeof(chunk)) ? bytes_avail : static_cast<DWORD>(sizeof(chunk));
                    if (ReadFile(pipe, chunk, to_read, &bytes_read, nullptr) && bytes_read > 0) {
                        if (req_buffer.size() + bytes_read > kMaxIpcRequestBytes) {
                            req_buffer.clear();
                            break;
                        }
                        req_buffer.append(chunk, bytes_read);
                        if (req_buffer.find('\n') != std::string::npos) {
                            break;
                        }
                    } else {
                        break;
                    }
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    total_waited_ms += 10;
                    if (total_waited_ms >= 5000) {
                        break;  // Timeout idle client
                    }
                }
            }

            auto line_end = req_buffer.find('\n');
            std::string line = (line_end != std::string::npos) ? req_buffer.substr(0, line_end) : req_buffer;

            if (!line.empty()) {
                auto parsed_req = parse_request(line);
                Response resp;
                if (!parsed_req) {
                    resp = Response::make_error(nullptr, parsed_req.error().code, parsed_req.error().message);
                } else {
                    resp = handle_rpc_call(parsed_req.value());
                }

                std::string resp_str = serialize_response(resp) + "\n";
                DWORD bytes_written = 0;
                WriteFile(pipe, resp_str.data(), static_cast<DWORD>(resp_str.size()), &bytes_written, nullptr);
                FlushFileBuffers(pipe);
            }

            DisconnectNamedPipe(pipe);
        }
        CloseHandle(pipe);
    }

    if (p_sd) {
        LocalFree(p_sd);
    }
#else
    ::unlink(endpoint_.c_str());

    server_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd_ < 0) return;

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, endpoint_.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(server_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(server_fd_);
        server_fd_ = -1;
        return;
    }

    ::chmod(endpoint_.c_str(), 0600);
    ::listen(server_fd_, 16);

    while (running_) {
        struct pollfd pfd{};
        pfd.fd = server_fd_;
        pfd.events = POLLIN;

        int ret = ::poll(&pfd, 1, 200);
        if (ret > 0 && (pfd.revents & POLLIN)) {
            int client_fd = ::accept(server_fd_, nullptr, nullptr);
            if (client_fd >= 0) {
                struct timeval tv{};
                tv.tv_sec = 5;
                tv.tv_usec = 0;
                ::setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                ::setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
                std::string req_buffer;
                char chunk[1024];
                ssize_t n = 0;
                while ((n = ::read(client_fd, chunk, sizeof(chunk))) > 0) {
                    if (req_buffer.size() + static_cast<std::size_t>(n) > kMaxIpcRequestBytes) {
                        req_buffer.clear();
                        break;
                    }
                    req_buffer.append(chunk, static_cast<std::size_t>(n));
                    if (req_buffer.find('\n') != std::string::npos) break;
                }

                auto line_end = req_buffer.find('\n');
                std::string line = (line_end != std::string::npos) ? req_buffer.substr(0, line_end) : req_buffer;

                if (!line.empty()) {
                    auto parsed_req = parse_request(line);
                    Response resp;
                    if (!parsed_req) {
                        resp = Response::make_error(nullptr, parsed_req.error().code, parsed_req.error().message);
                    } else {
                        resp = handle_rpc_call(parsed_req.value());
                    }

                    std::string resp_str = serialize_response(resp) + "\n";
                    (void)::write(client_fd, resp_str.data(), resp_str.size());
                }
                ::close(client_fd);
            }
        }
    }

    if (server_fd_ >= 0) {
        ::close(server_fd_);
        server_fd_ = -1;
        ::unlink(endpoint_.c_str());
    }
#endif
}

} // namespace ldns::ipc
