#pragma once

// Central logger (SPEC §16). All modules log through this; the UI never gets
// spdlog objects directly. Never log secrets (PSK, keys, tokens).

#include <memory>
#include <string>

#include <spdlog/spdlog.h>

namespace ldns {

// Creates the shared logger. `level` is one of trace/debug/info/warn/error/off
// (case-insensitive). Safe to call more than once; the last call wins.
void init_logger(const std::string& level = "info");

// Returns the process-wide logger (initialized with defaults on first use).
std::shared_ptr<spdlog::logger> logger();

}  // namespace ldns
