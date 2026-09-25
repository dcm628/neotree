#include "neo_tree_safety.hpp"

#include "hardware/structs/watchdog.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

namespace {

constexpr uint32_t scratch_magic = 0x4e544231;   // "NTB1"
// scratch[1] value marking a crash-loop escape into BOOTSEL, so the boot
// after the reflash still reports the fault that caused it.
constexpr uint32_t escaped_marker = 0xE5CA9E00;
constexpr uint32_t watchdog_timeout_ms = 8000;
constexpr uint32_t crashes_before_bootsel = 3;
constexpr uint64_t stable_after_us = 60'000'000;
// Core1 counts as stuck if its loop counter hasn't moved for this long
// (checked only once it has started looping).
constexpr uint64_t core1_stuck_us = 5'000'000;

safety_report_t report = {false, 0, 0, 0, -1};
bool stable = false;
uint32_t last_core1_loops = 0;
uint64_t core1_moved_us = 0;
volatile bool faulted = false;   // set by the fault handler on either core
bool stop_feeding = false;

// scratch[0] magic, [1] consecutive crash count, [2] fault pc, [3] fault lr
// with the faulting core in bit 0 (lr's own bit 0 is always 1 in Thumb).
volatile uint32_t *const scratch = watchdog_hw->scratch;

}  // namespace

void safety_boot()
{
    bool valid = scratch[0] == scratch_magic;
    bool escaped = valid && scratch[1] == escaped_marker;
    report.crash_reboot = watchdog_caused_reboot();
    report.crash_count = report.crash_reboot ? (valid && !escaped ? scratch[1] : 0) + 1 : 0;
    // A recorded fault counts only after a crash: this boot followed an
    // unplanned reset, or a crash-loop escape to BOOTSEL (and reflash).
    // Anything else there is noise, e.g. from a core stopped mid-flash-access
    // by a deliberate BOOTSEL.
    uint32_t pc = valid && (report.crash_reboot || escaped) ? scratch[2] : 0;
    uint32_t lr = valid && (report.crash_reboot || escaped) ? scratch[3] : 0;
    if (pc != 0)
    {
        report.fault_pc = pc;
        report.fault_lr = lr | 1u;
        report.fault_core = (int8_t)(lr & 1u);
    }
    scratch[0] = scratch_magic;
    scratch[1] = report.crash_count;
    scratch[2] = 0;
    scratch[3] = 0;

    if (report.crash_count >= crashes_before_bootsel)
    {
        scratch[1] = escaped_marker;   // a fresh flash starts counting again
        scratch[2] = pc;                // keep the last fault for the next boot to report
        scratch[3] = lr;
        reset_usb_boot(0, 0);           // core1 isn't running yet at this point
    }
    watchdog_enable(watchdog_timeout_ms, true);
}

void safety_poll(uint64_t now_us, uint32_t core1_loops)
{
    if (core1_loops != last_core1_loops)
    {
        last_core1_loops = core1_loops;
        core1_moved_us = now_us;
    }
    bool core1_ok = core1_loops == 0 || now_us - core1_moved_us < core1_stuck_us;
    if (core1_ok && !faulted && !stop_feeding)
    {
        watchdog_update();
    }
    if (!stable && now_us >= stable_after_us)
    {
        stable = true;
        scratch[1] = 0;
    }
}

void safety_stop_feeding()
{
    stop_feeding = true;
}

void safety_reboot_to_bootsel()
{
    // Stop core1 first: it would fault as flash access goes away.
    multicore_reset_core1();
    scratch[1] = 0;
    scratch[2] = 0;
    scratch[3] = 0;
    reset_usb_boot(0, 0);
    while (true)
    {
    }
}

safety_report_t safety_report()
{
    return report;
}

// ---- stack high-water marks ----

extern "C" uint32_t __StackBottom, __StackTop, __StackOneBottom, __StackOneTop;

namespace {
constexpr uint32_t stack_pattern = 0x5AC0FFEE;

void stack_range(int core, uint32_t *&bottom, uint32_t *&top)
{
    bottom = core == 0 ? &__StackBottom : &__StackOneBottom;
    top = core == 0 ? &__StackTop : &__StackOneTop;
}
}  // namespace

void safety_paint_stacks()
{
    // Core0's own stack: everything below the current frame (with margin).
    uint32_t marker;
    uint32_t *sp = &marker - 32;
    for (uint32_t *p = &__StackBottom; p < sp; p++)
    {
        *p = stack_pattern;
    }
    // Core1's isn't in use yet.
    for (uint32_t *p = &__StackOneBottom; p < &__StackOneTop; p++)
    {
        *p = stack_pattern;
    }
}

uint32_t safety_stack_used(int core)
{
    uint32_t *bottom, *top;
    stack_range(core, bottom, top);
    uint32_t *p = bottom;
    while (p < top && *p == stack_pattern)
    {
        p++;
    }
    return (uint32_t)((top - p) * sizeof(uint32_t));
}

uint32_t safety_stack_size(int core)
{
    uint32_t *bottom, *top;
    stack_range(core, bottom, top);
    return (uint32_t)((top - bottom) * sizeof(uint32_t));
}

// ---- fault handler (both cores share the vector table) ----

extern "C" void __attribute__((used)) safety_fault(uint32_t *frame)
{
    faulted = true;
    scratch[2] = frame[6];                           // stacked pc
    scratch[3] = (frame[5] & ~1u) | get_core_num();  // stacked lr, core in bit 0
    // Wait for the watchdog (the other core stops feeding it too).
    while (true)
    {
    }
}

extern "C" void __attribute__((naked)) isr_hardfault()
{
    asm volatile("tst lr, #4\n"
                 "ite eq\n"
                 "mrseq r0, msp\n"
                 "mrsne r0, psp\n"
                 "b safety_fault\n");
}
