#include "doctest/doctest.h"
#include "neotree/clock.hpp"

using neotree::SimClock;

TEST_CASE("clock: ticks come due exactly, with no drift over long runs")
{
    SimClock clock(120);
    uint64_t ticks = 0;
    // One hour in uneven 60 fps frames (16666/16667 us): integer frame times.
    const uint64_t frames = 60ull * 3600;
    int64_t last = 0;
    for (uint64_t f = 1; f <= frames; f++)
    {
        int64_t t = static_cast<int64_t>(f * 1'000'000 / 60);
        uint32_t due = clock.accumulate(t - last);
        last = t;
        for (uint32_t i = 0; i < due; i++)
        {
            clock.advance_tick();
        }
        ticks += due;
    }
    CHECK(ticks == 120ull * 3600);
    CHECK(clock.ticks() == 120ull * 3600);
    CHECK(clock.time_us() == 3600ll * 1'000'000);
}

TEST_CASE("clock: partial ticks carry over")
{
    SimClock clock(100);   // 10 ms ticks
    CHECK(clock.accumulate(4'000) == 0);
    CHECK(clock.accumulate(4'000) == 0);
    CHECK(clock.accumulate(4'000) == 1);   // 12 ms total
    CHECK(clock.accumulate(8'000) == 1);   // 20 ms total
    CHECK(clock.accumulate(0) == 0);
    CHECK(clock.accumulate(-5) == 0);
}

TEST_CASE("clock: discard_pending drops the waiting remainder")
{
    SimClock clock(100);
    clock.accumulate(9'000);
    clock.discard_pending();
    CHECK(clock.accumulate(9'000) == 0);
}
