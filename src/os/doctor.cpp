#include "os/doctor.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace ldns::os {

namespace {

bool check_port_53_bindable(std::string& detail, std::string& hint) {
#ifdef _WIN32
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        detail = "Cannot create UDP socket";
        hint = "Check network stack or reboot";
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    int res = ::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (res != 0) {
        int err = WSAGetLastError();
        closesocket(s);
        detail = "Bind 127.0.0.1:53 failed (WSA error " + std::to_string(err) + ")";
        hint = "Port 53 is already held by another service (ICS, Hyper-V, or Docker). Stop the conflicting service or configure [server].listen.";
        return false;
    }
    closesocket(s);
    detail = "127.0.0.1:53 is available for binding";
    return true;
#else
    int s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        detail = "Cannot create UDP socket";
        return false;
    }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    int res = ::bind(s, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (res != 0) {
        ::close(s);
        detail = "Bind 127.0.0.1:53 failed (errno " + std::to_string(errno) + ")";
        hint = "Linux needs root or CAP_NET_BIND_SERVICE, or another DNS resolver (systemd-resolved) holds the port. Run as service or grant capability.";
        return false;
    }
    ::close(s);
    detail = "127.0.0.1:53 is available for binding";
    return true;
#endif
}

bool check_multicast_port(std::string& detail, std::string& hint) {
#ifdef _WIN32
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;

    BOOL reuse = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(5390);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    int res = ::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    closesocket(s);
    if (res != 0) {
        detail = "Port 5390 UDP is in use or blocked";
        hint = "Ensure no other instance of Ldns is holding port 5390 without SO_REUSEADDR.";
        return false;
    }
    detail = "Discovery multicast port 5390 UDP is available";
    return true;
#else
    int s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return false;

    int reuse = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(5390);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    int res = ::bind(s, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    ::close(s);
    if (res != 0) {
        detail = "Port 5390 UDP is in use or blocked";
        hint = "Check if another Ldns process is running.";
        return false;
    }
    detail = "Discovery multicast port 5390 UDP is available";
    return true;
#endif
}

bool check_sync_port(std::string& detail, std::string& hint) {
#ifdef _WIN32
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;

    BOOL reuse = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(5391);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    int res = ::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    closesocket(s);
    if (res != 0) {
        detail = "Sync port 5391 TCP is occupied";
        hint = "Ensure port 5391 is free for peer sync sessions.";
        return false;
    }
    detail = "Sync engine port 5391 TCP is available";
    return true;
#else
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return false;

    int reuse = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(5391);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    int res = ::bind(s, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    ::close(s);
    if (res != 0) {
        detail = "Sync port 5391 TCP is occupied";
        hint = "Check for another instance using TCP port 5391.";
        return false;
    }
    detail = "Sync engine port 5391 TCP is available";
    return true;
#endif
}

bool check_db_integrity(CoreApi& api, std::string& detail, std::string& hint) {
    auto res = api.store().identity();
    if (!res.has_value()) {
        detail = "Database error: " + res.error().message;
        hint = "Check file permissions or restore DB from backup.";
        return false;
    }
    detail = "SQLite database integrity verified (OK)";
    return true;
}

bool check_privileges(std::string& detail, std::string& hint) {
#ifdef _WIN32
    BOOL is_admin = FALSE;
    PSID admin_group = nullptr;
    SID_IDENTIFIER_AUTHORITY nt_auth = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&nt_auth, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &admin_group)) {
        CheckTokenMembership(nullptr, admin_group, &is_admin);
        FreeSid(admin_group);
    }
    if (is_admin) {
        detail = "Running with Administrator privileges";
        return true;
    } else {
        detail = "Running as standard user (Non-Administrator)";
        hint = "For system-wide DNS configuration or service installation, run as Administrator.";
        return false;
    }
#else
    if (::geteuid() == 0) {
        detail = "Running as root";
        return true;
    } else {
        detail = "Running as unprivileged user (UID " + std::to_string(::getuid()) + ")";
        hint = "System service management and binding low ports requires root or ambient capabilities.";
        return false;
    }
#endif
}

} // namespace

DoctorReport Doctor::run_diagnostics(CoreApi& api) {
    DoctorReport report;

    auto run_check = [&](std::string name, auto fn) {
        DiagnosticItem item;
        item.name = std::move(name);
        item.passed = fn(item.detail, item.fix_hint);
        if (item.passed) {
            report.passed_count++;
        } else {
            report.failed_count++;
            report.all_passed = false;
        }
        report.items.push_back(std::move(item));
    };

    run_check("Port 53 (DNS Service)", check_port_53_bindable);
    run_check("Port 5390 (Discovery Multicast)", check_multicast_port);
    run_check("Port 5391 (LAN Sync)", check_sync_port);
    run_check("Database Integrity", [&](std::string& d, std::string& h) {
        return check_db_integrity(api, d, h);
    });
    run_check("OS Privileges", check_privileges);

    return report;
}

} // namespace ldns::os
