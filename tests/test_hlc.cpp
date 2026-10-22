#include "doctest.hpp"

#include "core/util/hlc.hpp"

using namespace ldns;

TEST_CASE("hlc field packing") {
    const HlcTimestamp ts = hlc_make(123456789ull, 42);
    CHECK(hlc_physical_ms(ts) == 123456789ull);
    CHECK(hlc_counter(ts) == 42);
}

TEST_CASE("HlcClock::now is strictly monotonic") {
    HlcClock clock(1000);
    HlcTimestamp previous = clock.now();
    for (int i = 0; i < 1000; ++i) {
        const HlcTimestamp current = clock.now();
        CHECK(current > previous);
        previous = current;
    }
}

TEST_CASE("HlcClock never moves backwards when wall clock is behind") {
    // Seed far in the future; then now() must stay at/above the seed.
    const std::uint64_t future = 4'000'000'000'000ull;
    HlcClock clock(future);
    const HlcTimestamp ts = clock.now();
    CHECK(hlc_physical_ms(ts) >= future);
}

TEST_CASE("HlcClock::receive advances past remote timestamps") {
    HlcClock clock(1);
    const HlcTimestamp remote = hlc_make(9'000'000'000'000ull, 7);
    const HlcTimestamp merged = clock.receive(remote);
    CHECK(merged > remote);
    CHECK(hlc_physical_ms(merged) == 9'000'000'000'000ull);
}

TEST_CASE("receive with equal max timestamps does not wrap the counter") {
    // Regression: the old code masked the 16-bit counter with & 0xFFFF, so a
    // remote counter of 0xFFFF made receive() return a *smaller* timestamp.
    const std::uint64_t physical = 4'000'000'010'000ull;
    HlcClock clock(hlc_make(physical, kHlcCounterMask));
    const HlcTimestamp remote = hlc_make(physical, kHlcCounterMask);
    const HlcTimestamp merged = clock.receive(remote);
    CHECK(merged > remote);
    CHECK(hlc_physical_ms(merged) >= physical);
}

TEST_CASE("receive with remote counter at max still increases") {
    const std::uint64_t physical = 4'000'000'020'000ull;
    HlcClock clock(hlc_make(physical, 0));
    const HlcTimestamp remote = hlc_make(physical, kHlcCounterMask);
    const HlcTimestamp merged = clock.receive(remote);
    CHECK(merged > remote);
}

TEST_CASE("HlcClock counter overflow borrows a millisecond") {
    const std::uint64_t physical = 5'000'000'000'000ull;
    HlcClock clock(hlc_make(physical, kHlcCounterMask));
    // Wall clock is behind the seeded physical time, so now() must bump.
    const HlcTimestamp ts = clock.now();
    CHECK(hlc_physical_ms(ts) >= physical);
    CHECK(ts > hlc_make(physical, kHlcCounterMask));
}

TEST_CASE("hlc_to_string is readable") {
    CHECK(hlc_to_string(hlc_make(123, 4)) == "123.4");
}
