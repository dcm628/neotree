#include "neo_tree_loop_monitor.hpp"

#include "hardware/irq.h"
#include "hardware/structs/nvic.h"
#include "hardware/structs/timer.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

namespace {

// Written by core0 only; read by either core for status. The spinlock keeps
// a reader from seeing the ring half-updated.
spin_lock_t *lock = nullptr;
loop_monitor_stats_t stats = {};
loop_stall_t ring[loop_stall_log_size] = {};
size_t ring_next = 0;
size_t ring_count = 0;

uint64_t prev_begin_us = 0;
uint8_t prev_flags = 0;
uint8_t cur_flags = 0;

}  // namespace

void loop_monitor_init()
{
    lock = spin_lock_init(spin_lock_claim_unused(true));
    // Read on core0: each core has its own NVIC.
    stats.core0_irqs[0] = nvic_hw->iser[0];
    stats.core0_irqs[1] = nvic_hw->iser[1];
}

void loop_monitor_pass_begin(uint64_t now_us)
{
    prev_flags = cur_flags;
    cur_flags = 0;
    if (prev_begin_us == 0)
    {
        prev_begin_us = now_us;
        return;
    }
    uint32_t gap = (uint32_t)(now_us - prev_begin_us);
    prev_begin_us = now_us;

    uint32_t irq = spin_lock_blocking(lock);
    stats.passes++;
    // Every long gap goes in the recent list (with what the pass did), but
    // only unexplained ones - where the pass did no deliberate work, like
    // rendering a frame - count as stalls.
    const uint8_t deliberate = loop_pass_heartbeat | loop_pass_commands | loop_pass_frame_prep;
    if (gap >= loop_stall_threshold_us && (prev_flags & deliberate))
    {
        ring[ring_next] = {(uint32_t)(now_us / 1000), gap, prev_flags};
        ring_next = (ring_next + 1) % loop_stall_log_size;
        if (ring_count < loop_stall_log_size)
        {
            ring_count++;
        }
    }
    else if (gap >= loop_stall_threshold_us)
    {
        uint32_t at_ms = (uint32_t)(now_us / 1000);
        stats.stalls_500us++;
        if (gap >= 2000)
        {
            stats.stalls_2ms++;
        }
        if (gap >= 10000)
        {
            stats.stalls_10ms++;
        }
        if (gap > stats.max_gap_us)
        {
            stats.max_gap_us = gap;
            stats.max_gap_at_ms = at_ms;
        }
        ring[ring_next] = {at_ms, gap, prev_flags};
        ring_next = (ring_next + 1) % loop_stall_log_size;
        if (ring_count < loop_stall_log_size)
        {
            ring_count++;
        }
    }
    spin_unlock(lock, irq);
}

namespace {

loop_irq_stats_t irq_stats[loop_irq_slots] = {{-1, 0, 0, 0, 0}, {-1, 0, 0, 0, 0}, {-1, 0, 0, 0, 0}, {-1, 0, 0, 0, 0}};
irq_handler_t irq_original[loop_irq_slots] = {};

template <size_t Slot>
void __not_in_flash_func(irq_wrapper)()
{
    uint32_t t0 = timer_hw->timerawl;
    irq_original[Slot]();
    uint32_t dt = timer_hw->timerawl - t0;
    loop_irq_stats_t &s = irq_stats[Slot];
    s.calls++;
    s.total_us += dt;
    if (dt > s.max_us)
    {
        s.max_us = dt;
    }
    if (dt >= loop_stall_threshold_us)
    {
        s.slow++;
    }
}

const irq_handler_t irq_wrappers[loop_irq_slots] = {irq_wrapper<0>, irq_wrapper<1>, irq_wrapper<2>, irq_wrapper<3>};

}  // namespace

void loop_monitor_wrap_core0_irqs()
{
    // Writes the RAM vector table directly: the handlers are already
    // installed (some exclusively), so the irq_* setters would refuse.
    irq_handler_t *vtable = (irq_handler_t *)scb_hw->vtor;
    size_t slot = 0;
    for (uint irq = 0; irq < NUM_IRQS && slot < loop_irq_slots; irq++)
    {
        if (!(nvic_hw->iser[irq / 32] & (1u << (irq % 32))))
        {
            continue;
        }
        uint32_t save = save_and_disable_interrupts();
        irq_original[slot] = vtable[VTABLE_FIRST_IRQ + irq];
        irq_stats[slot].irq = (int16_t)irq;
        vtable[VTABLE_FIRST_IRQ + irq] = irq_wrappers[slot];
        restore_interrupts(save);
        slot++;
    }
}

size_t loop_monitor_irq_stats(loop_irq_stats_t *out, size_t max)
{
    size_t n = 0;
    for (size_t i = 0; i < loop_irq_slots && n < max; i++)
    {
        if (irq_stats[i].irq >= 0)
        {
            out[n++] = irq_stats[i];
        }
    }
    return n;
}

void loop_monitor_mark(uint8_t flags)
{
    cur_flags |= flags;
}

loop_monitor_stats_t loop_monitor_stats()
{
    uint32_t irq = spin_lock_blocking(lock);
    loop_monitor_stats_t s = stats;
    spin_unlock(lock, irq);
    return s;
}

size_t loop_monitor_recent(loop_stall_t *out, size_t max)
{
    uint32_t irq = spin_lock_blocking(lock);
    size_t n = ring_count < max ? ring_count : max;
    for (size_t i = 0; i < n; i++)
    {
        out[i] = ring[(ring_next + loop_stall_log_size - 1 - i) % loop_stall_log_size];
    }
    spin_unlock(lock, irq);
    return n;
}
