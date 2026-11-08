#include <functional>
#pragma once

#include <string>

#include "core/util/result.hpp"

namespace ldns::os {

enum class ServiceState {
    Stopped,
    Starting,
    Running,
    Stopping,
    NotInstalled,
    Unknown
};

struct ServiceStatusInfo {
    ServiceState state{ServiceState::NotInstalled};
    std::string detail;
    bool is_installed{false};
};

class ServiceManager {
public:
#ifdef _WIN32
    /// Runs as a Windows Service managed by Service Control Manager.
    static int run_service(std::function<int()> runner, std::function<void()> on_stop);
#endif

    /// Returns current state of the Ldns service.
    static ServiceStatusInfo status();

    /// Installs Ldns as a system service.
    /// @param binary_path Path to the ldns executable. If empty, resolves current binary.
    static Result<void> install(const std::string& binary_path = "");

    /// Uninstalls the Ldns system service.
    static Result<void> uninstall();

    /// Starts the installed Ldns service.
    static Result<void> start();

    /// Stops the running Ldns service.
    static Result<void> stop();
};

} // namespace ldns::os
