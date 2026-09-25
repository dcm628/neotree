#ifndef _NEO_TREE_SAFETY_HPP
#define _NEO_TREE_SAFETY_HPP

// Keeps a bad firmware build from stranding the tree. The Pico is flashed
// remotely through the Pi over USB, and USB is serviced by core0; if core0
// crashes, nothing remote can reach it (this happened on 2026-09-24).
//
//   - Hardware watchdog: core0's main loop feeds it only while both cores are
//     making progress; a crash or hang on either core resets the chip.
//   - Crash reports: a fault handler saves the faulting address in the
//     watchdog scratch registers (which survive the reset); the next boot
//     reports it in the event log and status JSON.
//   - Crash-loop escape: 3 unplanned resets without 60 s of stable running
//     in between and the Pico reboots into BOOTSEL, where the Pi's
//     pi_flash.py can reflash it with no physical access.
//
// Watchdog scratch registers 0-3 are used here (4-7 belong to the SDK and
// bootrom).

#include <cstdint>

struct safety_report_t
{
    bool crash_reboot;       // this boot followed an unplanned watchdog reset
    uint32_t crash_count;    // consecutive unplanned resets (0 once stable)
    uint32_t fault_pc;       // address that faulted, if a fault caused it (0 = hang, not fault)
    uint32_t fault_lr;
    int8_t fault_core;       // -1 = none recorded
};

// First thing in main(): counts crash reboots, escapes to BOOTSEL after too
// many, then starts the watchdog.
void safety_boot();

// Core0 main loop, every pass. Feeds the watchdog while core1 is making
// progress (core1_loops = its loop counter).
void safety_poll(uint64_t now_us, uint32_t core1_loops);

// Stops feeding the watchdog, for a deliberate watchdog_reboot().
void safety_stop_feeding();

// Reboots straight into BOOTSEL (the BOOTSEL protocol command).
[[noreturn]] void safety_reboot_to_bootsel();

safety_report_t safety_report();

// Stack high-water marks. safety_paint_stacks() fills both cores' stacks with
// a pattern (call first thing in main, before core1 starts); *_used() then
// scan for the deepest byte ever overwritten.
void safety_paint_stacks();
uint32_t safety_stack_used(int core);
uint32_t safety_stack_size(int core);

#endif
