#ifndef _NEO_TREE_EVENT_LOG_HPP
#define _NEO_TREE_EVENT_LOG_HPP

#include "pico/stdlib.h"

// Recent notable events (WiFi connects/failures, clients, mode changes,
// reboots...), timestamped, kept in a small ring so the app's debug page can
// show them (see neo_tree_status.hpp). Each event is also printed to USB
// serial. Callable from either core, but not from IRQ context (it printfs).

const size_t event_log_capacity = 24;
const size_t event_text_max = 72;

struct event_entry
{
    uint32_t t_ms;   // since boot
    char text[event_text_max];
};

// core0, before launching core1.
void event_log_init();

void event_logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Copies up to `max` entries, oldest first. Returns how many.
size_t event_log_snapshot(event_entry *out, size_t max);

// Events logged since boot (including ones that have fallen out of the ring).
uint32_t event_log_total();

#endif
