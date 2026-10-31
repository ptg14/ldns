#include "doctest.hpp"

#include "core/core_api.hpp"
#include "os/doctor.hpp"
#include "os/firewall.hpp"
#include "os/service.hpp"
#include "os/system_dns_manager.hpp"

#include <filesystem>

using namespace ldns;
using namespace ldns::os;

namespace {

struct TempDir {
    std::filesystem::path path;

    TempDir() {
        path = std::filesystem::temp_directory_path() /
               ("ldns-os-test-" + std::to_string(::rand()) + "-" +
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

} // namespace

TEST_CASE("System Doctor Diagnostics") {
    TempDir temp;
    auto api_res = CoreApi::create(options_for(temp));
    REQUIRE(api_res.has_value());
    auto& api = *api_res.value();

    auto report = Doctor::run_diagnostics(api);
    CHECK(report.items.size() == 5);
    CHECK(report.passed_count + report.failed_count == 5);

    // Database integrity must be OK on our freshly opened database
    bool found_db_check = false;
    for (const auto& item : report.items) {
        if (item.name == "Database Integrity") {
            found_db_check = true;
            CHECK(item.passed == true);
        }
    }
    CHECK(found_db_check == true);
}

TEST_CASE("System DNS Manager Status") {
    TempDir temp;
    auto api_res = CoreApi::create(options_for(temp));
    REQUIRE(api_res.has_value());
    auto& api = *api_res.value();

    SystemDnsManager manager(api);
    auto status = manager.status();

    // Verify non-empty detection info
    CHECK_FALSE(status.method_used.empty());
}

TEST_CASE("Firewall Status Inspection") {
    auto status = Firewall::status();
    // In our test environment, backend should be recognized (e.g. windows_advfirewall or ufw/firewalld)
    CHECK_FALSE(status.backend_detected.empty());
}

TEST_CASE("Service Manager Status Inspection") {
    auto status = ServiceManager::status();
    // By default in test environment, service might not be installed or stopped
    CHECK_FALSE(status.detail.empty());
}
