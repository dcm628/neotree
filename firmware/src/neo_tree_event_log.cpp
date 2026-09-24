#include "neo_tree_event_log.hpp"

#include <stdarg.h>
#include <stdio.h>
#include <cstring>

#include "hardware/sync.h"

static event_entry ring[event_log_capacity];
static uint32_t total = 0;   // ring index of the next write = total % capacity
static spin_lock_t *lock = nullptr;

void event_log_init()
{
    lock = spin_lock_init(spin_lock_claim_unused(true));
}

void event_logf(const char *fmt, ...)
{
    event_entry e;
    e.t_ms = (uint32_t)(time_us_64() / 1000);
    va_list args;
    va_start(args, fmt);
    vsnprintf(e.text, sizeof(e.text), fmt, args);
    va_end(args);
    printf("event: %s\n", e.text);

    uint32_t irq = spin_lock_blocking(lock);
    ring[total % event_log_capacity] = e;
    total++;
    spin_unlock(lock, irq);
}

size_t event_log_snapshot(event_entry *out, size_t max)
{
    uint32_t irq = spin_lock_blocking(lock);
    size_t count = total < event_log_capacity ? total : event_log_capacity;
    if (count > max)
    {
        count = max;
    }
    uint32_t first = total - count;   // oldest entry to copy
    for (size_t i = 0; i < count; i++)
    {
        out[i] = ring[(first + i) % event_log_capacity];
    }
    spin_unlock(lock, irq);
    return count;
}

uint32_t event_log_total()
{
    return total;
}
