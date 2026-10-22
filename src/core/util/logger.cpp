#include "core/util/logger.hpp"

#include <algorithm>
#include <cctype>
#include <memory>
#include <mutex>
#include <string>

#include <spdlog/sinks/stdout_color_sinks.h>

namespace ldns {
namespace {

std::mutex g_mutex;
std::shared_ptr<spdlog::logger> g_logger;

spdlog::level::level_enum parse_level(const std::string& text) {
    std::string lowered = text;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lowered == "trace") return spdlog::level::trace;
    if (lowered == "debug") return spdlog::level::debug;
    if (lowered == "warn" || lowered == "warning") return spdlog::level::warn;
    if (lowered == "error") return spdlog::level::err;
    if (lowered == "off" || lowered == "none") return spdlog::level::off;
    return spdlog::level::info;
}

// Caller must hold g_mutex. Kept separate so logger() cannot deadlock on the
// non-recursive mutex by calling init_logger().
std::shared_ptr<spdlog::logger> make_logger(const std::string& level) {
    auto sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    auto log = std::make_shared<spdlog::logger>("ldns", std::move(sink));
    log->set_level(parse_level(level));
    log->set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
    log->flush_on(spdlog::level::warn);
    return log;
}

}  // namespace

void init_logger(const std::string& level) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_logger = make_logger(level);
    spdlog::set_default_logger(g_logger);
}

std::shared_ptr<spdlog::logger> logger() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_logger) {
        g_logger = make_logger("info");
        spdlog::set_default_logger(g_logger);
    }
    return g_logger;
}

}  // namespace ldns
