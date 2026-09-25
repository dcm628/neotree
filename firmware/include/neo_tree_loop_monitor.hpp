#ifndef _NEO_TREE_LOOP_MONITOR_HPP
#define _NEO_TREE_LOOP_MONITOR_HPP

// Core0 main-loop stall detector. A pass of the main loop normally takes a
// few microseconds, so a long gap between consecutive passes means core0 was
// held up - by an interrupt, by blocking work in the pass itself, or by a bus
// or flash stall. Each long gap is recorded with what that pass was doing, so
// stalls can be told apart from work the loop did on purpose.

#include <cstddef>
#include <cstdint>

// What a main-loop pass did (bit flags).
enum loop_pass_flags : uint8_t
{
    loop_pass_heartbeat = 1u << 0,   // printed the serial heartbeat
    loop_pass_commands = 1u << 1,    // processed at least one command
    loop_pass_frame_prep = 1u << 2,  // ran the engine + packed a frame
    loop_pass_frame_start = 1u << 3, // started a frame's output
};

// Gaps at least this long are recorded.
constexpr uint32_t loop_stall_threshold_us = 500;
constexpr size_t loop_stall_log_size = 24;

struct loop_stall_t
{
    uint32_t at_ms;    // uptime when the pass after the gap began
    uint32_t gap_us;   // time from the previous pass's start to this one's
    uint8_t flags;     // loop_pass_flags of the previous pass
};

struct loop_monitor_stats_t
{
    uint32_t passes;
    uint32_t stalls_500us;   // unexplained gaps >= 0.5 ms (the pass did no deliberate work)
    uint32_t stalls_2ms;
    uint32_t stalls_10ms;
    uint32_t max_gap_us;
    uint32_t max_gap_at_ms;
    uint32_t core0_irqs[2];  // NVIC enabled-interrupt masks on core0
};

// Call from core0 once at startup (records core0's enabled IRQs).
void loop_monitor_init();

// Call at the top of every core0 main-loop pass, with that pass's time.
void loop_monitor_pass_begin(uint64_t now_us);

// Wraps each interrupt enabled on core0 so calls and time spent in its
// handler are measured. Call on core0 once everything that installs core0
// handlers (stdio, LED output, core1 launch) has run. The timer IRQ's call
// rate is the tell for the stall episodes (docs/RENDERER.md 15): ~17/s
// normally, ~5,900/s while they were happening.
constexpr size_t loop_irq_slots = 4;
struct loop_irq_stats_t
{
    int16_t irq;          // -1 = unused slot
    uint32_t calls;
    uint32_t max_us;
    uint32_t slow;        // calls >= loop_stall_threshold_us
    uint64_t total_us;
};
void loop_monitor_wrap_core0_irqs();
size_t loop_monitor_irq_stats(loop_irq_stats_t *out, size_t max);

// OR in what the current pass did.
void loop_monitor_mark(uint8_t flags);

loop_monitor_stats_t loop_monitor_stats();

// Copies the most recent stalls, newest first. Returns how many.
size_t loop_monitor_recent(loop_stall_t *out, size_t max);

#endif
