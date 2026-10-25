#include "core/core_api.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

using namespace ldns;

// Manual M2 benchmark: import N rows (default 100 000) and reopen the
// database, with per-phase timings. Not part of ctest.

namespace {

double elapsed_ms(std::chrono::steady_clock::time_point start,
                  std::chrono::steady_clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

using Clock = std::chrono::steady_clock;

struct Phase {
    const char* name;
    Clock::time_point start;
    explicit Phase(const char* label) : name(label), start(Clock::now()) {}
    ~Phase() {
        const auto end = Clock::now();
        std::fprintf(stderr, "  %-28s %8.1f ms\n", name, elapsed_ms(start, end));
    }
};

}  // namespace

int main(int argc, char** argv) {
    const int rows = argc > 1 ? std::atoi(argv[1]) : 100000;
    const auto dir = std::filesystem::temp_directory_path() / "ldns-bench";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    CoreApi::Options options;
    options.paths.config_file = (dir / "config.toml").string();
    options.paths.data_file = (dir / "ldns.db").string();

    std::string csv = "name,type,ttl,target\n";
    csv.reserve(static_cast<std::size_t>(rows) * 40);
    for (int i = 0; i < rows; ++i) {
        csv += "host" + std::to_string(i) + ".lan,A,60,10." +
               std::to_string((i / 65536) % 256) + "." + std::to_string((i / 256) % 256) + "." +
               std::to_string(i % 254 + 1) + "\n";
    }

    std::fprintf(stderr, "phases (rows=%d, csv=%zu B):\n", rows, csv.size());
    double import_ms = 0.0;
    std::size_t imported = 0;
    {
        auto api = CoreApi::create(options);
        if (!api) {
            std::fprintf(stderr, "create failed: %s\n", api.error().message.c_str());
            return 1;
        }
        const auto start = Clock::now();
        auto report = api.value()->import_text(csv, false, true);
        const auto end = Clock::now();
        if (!report) {
            std::fprintf(stderr, "import failed: %s\n", report.error().message.c_str());
            return 1;
        }
        import_ms = elapsed_ms(start, end);
        imported = report.value().imported;
    }

    double reopen_ms = 0.0;
    std::size_t zone_records = 0;
    {
        const auto start = Clock::now();
        auto reopened = CoreApi::create(options);
        const auto end = Clock::now();
        if (!reopened) {
            std::fprintf(stderr, "reopen failed: %s\n", reopened.error().message.c_str());
            return 1;
        }
        reopen_ms = elapsed_ms(start, end);
        zone_records = reopened.value()->zone().record_count();
    }

    std::fprintf(stderr,
                 "rows=%d imported=%zu import=%.0f ms (%.0f rows/s) reopen=%.0f ms zone=%zu\n",
                 rows, imported, import_ms,
                 static_cast<double>(imported) / (import_ms / 1000.0), reopen_ms, zone_records);
    std::filesystem::remove_all(dir);
    return 0;
}
