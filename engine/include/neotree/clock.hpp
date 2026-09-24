#pragma once
// Simulation clock: turns elapsed time into a whole number of fixed-length
// ticks. All engine time is simulation time (docs/RENDERER.md section 10) - it
// never reads a wall clock, so the host simulator can run it at any speed.

#include <cstdint>

namespace neotree {

class SimClock
{
public:
    explicit SimClock(uint32_t tick_hz = 120) : tick_hz_(tick_hz) {}

    // Adds dt_us of elapsed time and returns how many ticks are now due. The
    // remainder carries over exactly (the accumulator counts in units of
    // 1/(1e6 * tick_hz) s), so there's no drift over any run length.
    uint32_t accumulate(int64_t dt_us)
    {
        if (dt_us <= 0)
        {
            return 0;
        }
        accumulator_ += static_cast<uint64_t>(dt_us) * tick_hz_;
        uint64_t due = accumulator_ / us_per_s;
        accumulator_ -= due * us_per_s;
        return static_cast<uint32_t>(due);
    }

    // Called once per tick actually run.
    void advance_tick() { ticks_++; }

    // Drops whatever time is waiting (used when a host deliberately skips
    // ticks it couldn't run).
    void discard_pending() { accumulator_ = 0; }

    uint64_t ticks() const { return ticks_; }
    uint32_t tick_hz() const { return tick_hz_; }
    float tick_dt_s() const { return 1.0f / static_cast<float>(tick_hz_); }

    // Exact simulation time of the last completed tick.
    int64_t time_us() const { return static_cast<int64_t>(ticks_ * us_per_s / tick_hz_); }

private:
    static constexpr uint64_t us_per_s = 1'000'000;
    uint32_t tick_hz_;
    uint64_t ticks_ = 0;
    uint64_t accumulator_ = 0;
};

}  // namespace neotree
