#ifndef _NEO_TREE_STATUS_HPP
#define _NEO_TREE_STATUS_HPP

#include "pico/stdlib.h"

// JSON snapshot of the tree's internals for the app's debug page (and
// STATUS_REQUEST over USB serial): firmware/uptime/reset reason, LED output,
// command queue, network server + lwIP memory, WiFi metrics + connection
// trace, and the recent event log. JSON so new fields show up in the app
// without app changes.
//
// Reads other modules' state without locks - torn values are possible but
// harmless for diagnostics. Safe from either core and from lwIP IRQ context
// (it never talks to the WiFi chip - see wifi_snapshot()).

// Largest snapshot, with a full event log.
const size_t status_json_max = 4096;

// core0, before launching core1.
void status_init();

// Writes the snapshot into out (NUL-terminated). Returns its length. Entries
// that don't fit (e.g. old events) are left out rather than breaking the JSON.
size_t status_build_json(char *out, size_t cap);

// Set once at boot (main()): whether the last reset was the watchdog (e.g. a
// REBOOT command) rather than power-on or the reset pin.
extern bool status_boot_was_watchdog;

#endif
