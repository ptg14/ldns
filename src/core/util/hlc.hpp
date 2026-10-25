#pragma once

// Hybrid Logical Clock (SPEC §5.2, protocol.md §2.2).
// 48 high bits = Unix milliseconds, 16 low bits = logical counter.

#include <cstdint>
#include <mutex>
#include <string>

namespace ldns {

using HlcTimestamp = std::uint64_t;

constexpr std::uint64_t kHlcCounterMask = 0xFFFF;

constexpr std::uint64_t hlc_physical_ms(HlcTimestamp ts) noexcept { return ts >> 16; }
constexpr std::uint16_t hlc_counter(HlcTimestamp ts) noexcept {
    return static_cast<std::uint16_t>(ts & kHlcCounterMask);
}
constexpr HlcTimestamp hlc_make(std::uint64_t ms, std::uint16_t counter) noexcept {
    return (ms << 16) | counter;
}

// Monotonic clock for one process. Thread-safe: now()/receive() serialize on
// an internal mutex, because zone writes and the IP-change watcher may stamp
// events concurrently (SPEC §7.4.6).
class HlcClock {
public:
    explicit HlcClock(std::uint64_t physical_ms = 0) noexcept
        : last_(hlc_make(physical_ms, 0)) {}

    // Local event: never moves backwards even if the wall clock does.
    HlcTimestamp now();

    // Merge a remote timestamp (protocol.md §2.2). Returns the stamp to use
    // for the local event accompanying the received message.
    HlcTimestamp receive(HlcTimestamp remote);

    HlcTimestamp last() const {
        std::lock_guard lock(mutex_);
        return last_;
    }

private:
    mutable std::mutex mutex_;
    HlcTimestamp last_;
};

// Human-readable form for --trace and logs: "<ms>.<counter>".
std::string hlc_to_string(HlcTimestamp ts);

}  // namespace ldns
