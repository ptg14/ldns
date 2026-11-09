#include "os/service.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsvc.h>
#else
#include <unistd.h>
#endif

namespace ldns::os {

namespace {

std::string get_current_executable_path() {
#ifdef _WIN32
    char buffer[MAX_PATH];
    DWORD len = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
    if (len > 0) return std::string(buffer, len);
#else
    char buffer[1024];
    ssize_t len = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (len > 0) {
        buffer[len] = '\0';
        return std::string(buffer);
    }
#endif
    return "ldns";
}

} // namespace

#ifdef _WIN32
static SERVICE_STATUS_HANDLE g_status_handle = nullptr;
static SERVICE_STATUS g_service_status{};
static std::function<void()> g_stop_cb = nullptr;
static std::function<int()> g_service_runner = nullptr;

static DWORD WINAPI ServiceCtrlHandler(DWORD dwControl, DWORD /*dwEventType*/, LPVOID /*lpEventData*/, LPVOID /*lpContext*/) {
    switch (dwControl) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            g_service_status.dwCurrentState = SERVICE_STOP_PENDING;
            g_service_status.dwControlsAccepted = 0;
            SetServiceStatus(g_status_handle, &g_service_status);
            if (g_stop_cb) {
                g_stop_cb();
            }
            return NO_ERROR;
        default:
            return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

static VOID WINAPI ServiceMain(DWORD /*argc*/, LPSTR* /*argv*/) {
    g_status_handle = RegisterServiceCtrlHandlerExA("Ldns", ServiceCtrlHandler, nullptr);
    if (!g_status_handle) return;

    g_service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_service_status.dwCurrentState = SERVICE_START_PENDING;
    g_service_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    g_service_status.dwWin32ExitCode = 0;
    SetServiceStatus(g_status_handle, &g_service_status);

    g_service_status.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(g_status_handle, &g_service_status);

    int exit_code = 0;
    if (g_service_runner) {
        exit_code = g_service_runner();
    }

    g_service_status.dwCurrentState = SERVICE_STOPPED;
    g_service_status.dwWin32ExitCode = exit_code;
    SetServiceStatus(g_status_handle, &g_service_status);
}

int ServiceManager::run_service(std::function<int()> runner, std::function<void()> on_stop) {
    g_service_runner = std::move(runner);
    g_stop_cb = std::move(on_stop);

    SERVICE_TABLE_ENTRYA service_table[] = {
        { const_cast<LPSTR>("Ldns"), ServiceMain },
        { nullptr, nullptr }
    };

    if (!StartServiceCtrlDispatcherA(service_table)) {
        return static_cast<int>(GetLastError());
    }
    return 0;
}
#endif

ServiceStatusInfo ServiceManager::status() {
    ServiceStatusInfo info;
#ifdef _WIN32
    SC_HANDLE scm = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        info.detail = "Cannot open Service Control Manager";
        return info;
    }

    SC_HANDLE svc = OpenServiceA(scm, "Ldns", SERVICE_QUERY_STATUS);
    if (!svc) {
        CloseServiceHandle(scm);
        info.state = ServiceState::NotInstalled;
        info.detail = "Service is not installed";
        return info;
    }

    SERVICE_STATUS_PROCESS ssp{};
    DWORD needed = 0;
    if (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&ssp),
                             sizeof(ssp), &needed)) {
        info.is_installed = true;
        switch (ssp.dwCurrentState) {
            case SERVICE_RUNNING:
                info.state = ServiceState::Running;
                info.detail = "Service is running (PID " + std::to_string(ssp.dwProcessId) + ")";
                break;
            case SERVICE_STOPPED:
                info.state = ServiceState::Stopped;
                info.detail = "Service is stopped";
                break;
            case SERVICE_START_PENDING:
                info.state = ServiceState::Starting;
                info.detail = "Service is starting";
                break;
            case SERVICE_STOP_PENDING:
                info.state = ServiceState::Stopping;
                info.detail = "Service is stopping";
                break;
            default:
                info.state = ServiceState::Unknown;
                info.detail = "Status unknown";
                break;
        }
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
#else
    if (std::system("systemctl is-active --quiet ldns") == 0) {
        info.is_installed = true;
        info.state = ServiceState::Running;
        info.detail = "systemd unit ldns is active (running)";
    } else if (std::system("systemctl is-enabled --quiet ldns 2>/dev/null") == 0) {
        info.is_installed = true;
        info.state = ServiceState::Stopped;
        info.detail = "systemd unit ldns is installed but stopped";
    } else {
        info.is_installed = false;
        info.state = ServiceState::NotInstalled;
        info.detail = "systemd unit ldns is not installed";
    }
#endif
    return info;
}

Result<void> ServiceManager::install(const std::string& binary_path) {
    const std::string bin = binary_path.empty() ? get_current_executable_path() : binary_path;

#ifdef _WIN32
    SC_HANDLE scm = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!scm) {
        return Error{4, "Failed to open SCM. Run as Administrator."};
    }

    std::string bin_with_args = "\"" + bin + "\" daemon --service";
    SC_HANDLE svc = CreateServiceA(
        scm,
        "Ldns",
        "Ldns Peer-to-Peer DNS",
        SERVICE_ALL_ACCESS,
        SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START,
        SERVICE_ERROR_NORMAL,
        bin_with_args.c_str(),
        nullptr, nullptr, nullptr, nullptr, nullptr);

    if (!svc) {
        DWORD err = GetLastError();
        CloseServiceHandle(scm);
        if (err == ERROR_SERVICE_EXISTS) {
            return Error{5, "Ldns service is already installed."};
        }
        return Error{4, "Failed to create service (Error " + std::to_string(err) + "). Run as Administrator."};
    }

    // Set failure actions: restart service after crash (SPEC §13.3)
    SC_ACTION actions[2];
    actions[0].Type = SC_ACTION_RESTART;
    actions[0].Delay = 2000;
    actions[1].Type = SC_ACTION_RESTART;
    actions[1].Delay = 5000;

    SERVICE_FAILURE_ACTIONSA sfa{};
    sfa.dwResetPeriod = 86400; // 1 day
    sfa.cActions = 2;
    sfa.lpsaActions = actions;
    ChangeServiceConfig2A(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &sfa);

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
#else
    std::string unit_content =
        "[Unit]\n"
        "Description=Ldns Peer-to-Peer Local DNS\n"
        "After=network.target\n\n"
        "[Service]\n"
        "Type=simple\n"
        "ExecStart=\"" + bin + "\" daemon --foreground\n"
        "Restart=always\n"
        "RestartSec=3\n"
        "AmbientCapabilities=CAP_NET_BIND_SERVICE\n"
        "NoNewPrivileges=true\n\n"
        "[Install]\n"
        "WantedBy=multi-user.target\n";

    std::ofstream file("/etc/systemd/system/ldns.service");
    if (!file.is_open()) {
        return Error{4, "Cannot write /etc/systemd/system/ldns.service. Run as root."};
    }
    file << unit_content;
    file.close();

    [[maybe_unused]] auto r_dr1 = std::system("systemctl daemon-reload >/dev/null 2>&1");
    [[maybe_unused]] auto r_en = std::system("systemctl enable ldns >/dev/null 2>&1");
#endif
    return {};
}

Result<void> ServiceManager::uninstall() {
#ifdef _WIN32
    SC_HANDLE scm = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scm) {
        return Error{4, "Failed to open SCM. Run as Administrator."};
    }

    SC_HANDLE svc = OpenServiceA(scm, "Ldns", SERVICE_STOP | DELETE);
    if (!svc) {
        CloseServiceHandle(scm);
        return Error{1, "Ldns service is not installed."};
    }

    SERVICE_STATUS ss;
    ControlService(svc, SERVICE_CONTROL_STOP, &ss);

    if (!DeleteService(svc)) {
        DWORD err = GetLastError();
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return Error{4, "Failed to delete service (Error " + std::to_string(err) + ")."};
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
#else
    [[maybe_unused]] auto r_st = std::system("systemctl stop ldns >/dev/null 2>&1");
    [[maybe_unused]] auto r_dis = std::system("systemctl disable ldns >/dev/null 2>&1");
    ::unlink("/etc/systemd/system/ldns.service");
    [[maybe_unused]] auto r_dr1 = std::system("systemctl daemon-reload >/dev/null 2>&1");
#endif
    return {};
}

Result<void> ServiceManager::start() {
#ifdef _WIN32
    SC_HANDLE scm = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scm) return Error{4, "Failed to open SCM. Run as Administrator."};

    SC_HANDLE svc = OpenServiceA(scm, "Ldns", SERVICE_START);
    if (!svc) {
        CloseServiceHandle(scm);
        return Error{1, "Ldns service is not installed."};
    }

    if (!StartServiceA(svc, 0, nullptr)) {
        DWORD err = GetLastError();
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        if (err == ERROR_SERVICE_ALREADY_RUNNING) return {};
        return Error{1, "Failed to start service (Error " + std::to_string(err) + ")."};
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
#else
    int res = std::system("systemctl start ldns");
    if (res != 0) {
        return Error{1, "Failed to start ldns systemd service."};
    }
#endif
    return {};
}

Result<void> ServiceManager::stop() {
#ifdef _WIN32
    SC_HANDLE scm = OpenSCManagerA(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scm) return Error{4, "Failed to open SCM. Run as Administrator."};

    SC_HANDLE svc = OpenServiceA(scm, "Ldns", SERVICE_STOP);
    if (!svc) {
        CloseServiceHandle(scm);
        return Error{1, "Ldns service is not installed."};
    }

    SERVICE_STATUS ss;
    if (!ControlService(svc, SERVICE_CONTROL_STOP, &ss)) {
        DWORD err = GetLastError();
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return Error{1, "Failed to stop service (Error " + std::to_string(err) + ")."};
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
#else
    int res = std::system("systemctl stop ldns");
    if (res != 0) {
        return Error{1, "Failed to stop ldns systemd service."};
    }
#endif
    return {};
}

} // namespace ldns::os
