#ifndef _NEO_TREE_CLOCK_HPP
#define _NEO_TREE_CLOCK_HPP

// The tree's wall clock. UTC comes from SNTP over WiFi (pool.ntp.org, then
// time.google.com; at boot and hourly), or - as a fallback when SNTP hasn't
// worked for a while - from the app (TIME_SET). It's kept as an offset from
// the microsecond timer since boot, so it's lost at power-off until the next
// sync. Local time comes from a POSIX time-zone rule, which carries the
// daylight-saving changes (neotree/civil_time.hpp): the tree's home zone,
// Los Angeles, unless one is set (TIME_ZONE, from the app's Debug page) and
// stored in flash. NTP carries no time zone, so it's known from power-up
// without waiting for a phone.

#include <cstddef>
#include <cstdint>

#include "neotree/civil_time.hpp"

enum class clock_source : uint8_t
{
    none,   // not set since power-up
    sntp,
    app,
};

struct clock_status_t
{
    bool set;
    clock_source source;
    int64_t unix_ms;
    uint32_t sync_age_s;       // since the last accepted time, any source
    uint32_t sntp_syncs;
    uint32_t sntp_age_s;       // since the last SNTP sync (UINT32_MAX: never)
    uint32_t app_sets;         // TIME_SETs accepted
    uint32_t app_ignored;      // TIME_SETs ignored (SNTP recent)
    int32_t last_step_ms;      // how far the last sync moved an already-set clock
    char tz[64];               // the POSIX rule in use
    bool tz_stored;            // set and stored, rather than the default
};

// The tree's home: Los Angeles (US Pacific, DST from the second Sunday in
// March to the first in November).
constexpr char clock_default_tz[] = "PST8PDT,M3.2.0,M11.1.0";

// core0 at boot, before core1 starts: the stored time zone.
void clock_init();
// core1's loop: starts SNTP once the WiFi link is up.
void clock_poll_core1();

// UTC now, in microseconds since 1970. False (and 0) until set. Any core.
bool clock_unix_us(int64_t *out);
// Local time now. False until the clock is set. Any core.
bool clock_local(neotree::CivilTime *out);
// The time zone in use (a copy). Any core.
void clock_zone(neotree::TimeZone *out);

// The app's time (Unix ms): taken unless SNTP synced in the last 2 hours.
// Returns whether it was taken. Any core.
bool clock_set_from_app(int64_t unix_ms);
// A POSIX time-zone rule: checked, used at once, and stored in flash if it
// changed; empty (len 0) goes back to the default. False if it isn't a
// valid rule. core0 (writes flash).
bool clock_set_zone(const char *tz, size_t len);

clock_status_t clock_status();

#endif
