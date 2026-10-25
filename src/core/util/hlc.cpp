#include "core/util/hlc.hpp"

#include <algorithm>
#include <chrono>

namespace ldns {
namespace {

std::uint64_t wall_clock_ms() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

}  // namespace

HlcTimestamp HlcClock::now() {
    std::lock_guard lock(mutex_);
    const std::uint64_t physical = wall_clock_ms();
    const std::uint64_t last_physical = hlc_physical_ms(last_);

    if (physical > last_physical) {
        last_ = hlc_make(physical, 0);
    } else if (hlc_counter(last_) < kHlcCounterMask) {
        last_ = hlc_make(last_physical, static_cast<std::uint16_t>(hlc_counter(last_) + 1));
    } else {
        // Counter exhausted: borrow one millisecond so ordering is preserved.
        last_ = hlc_make(last_physical + 1, 0);
    }
    return last_;
}

namespace {

HlcTimestamp bump_counter(std::uint64_t physical, std::uint32_t counter) {
    if (counter > kHlcCounterMask) return hlc_make(physical + 1, 0);
    return hlc_make(physical, static_cast<std::uint16_t>(counter));
}

}  // namespace

HlcTimestamp HlcClock::receive(HlcTimestamp remote) {
    std::lock_guard lock(mutex_);
    // Standard HLC merge (Kulkarni et al.): l' = max(l, l_m, pt), then the
    // counter is taken from whichever source supplied l'.
    const std::uint64_t physical = wall_clock_ms();
    const std::uint64_t local_physical = hlc_physical_ms(last_);
    const std::uint64_t remote_physical = hlc_physical_ms(remote);
    const std::uint64_t merged = std::max({physical, local_physical, remote_physical});

    if (merged == local_physical && merged == remote_physical) {
        const auto counter =
            std::max<std::uint32_t>(hlc_counter(last_), hlc_counter(remote)) + 1;
        last_ = bump_counter(merged, counter);
    } else if (merged == local_physical) {
        last_ = bump_counter(merged, static_cast<std::uint32_t>(hlc_counter(last_)) + 1);
    } else if (merged == remote_physical) {
        last_ = bump_counter(merged, static_cast<std::uint32_t>(hlc_counter(remote)) + 1);
    } else {
        last_ = hlc_make(merged, 0);
    }
    return last_;
}

std::string hlc_to_string(HlcTimestamp ts) {
    return std::to_string(hlc_physical_ms(ts)) + "." + std::to_string(hlc_counter(ts));
}

}  // namespace ldns
