#pragma once
// The schedule: lights on and off at set times on chosen days, like a
// plug-in Christmas light timer, and events at set moments (to the second) -
// a show at a minute to midnight on New Year's Eve. All in local wall-clock
// time (neotree/civil_time.hpp); the director runs it
// (Director::run_schedule) and the library stores it.
//
// The on/off timer acts only when its on/off state changes (or the schedule
// is edited, or the clock is first known): turning the lights on or off by
// hand lasts until its next change. An event turns the lights on while it
// runs, and after its length the tree goes back to what was playing before.

#include <cstddef>
#include <cstdint>

#include "neotree/scene_spec.hpp"

namespace neotree {

constexpr uint8_t max_timers = 8;
constexpr uint8_t max_scheduled_events = 12;
constexpr uint8_t every_day = 0x7F;          // bit 0 = Sunday .. bit 6 = Saturday
constexpr size_t target_size = 24;           // a preset / show name, or a mode id ("fx:" + a name)

// Lights on at on_s, off at off_s (seconds into the local day), starting on
// the days in `days`. An off at or before the on is the next day (on at
// 17:00, off at 01:00).
struct TimerRule
{
    bool enabled = true;
    uint8_t days = every_day;
    uint32_t on_s = 17 * 3600;
    uint32_t off_s = 23 * 3600;
};

enum class Repeat : uint8_t
{
    once,     // on year-month-day
    yearly,   // on month-day
    weekly,   // on the days in `days`
};

enum class EventAction : uint8_t
{
    preset,   // a scene by name
    show,     // a show by name
    mode,     // a mode (built-in or custom effect) by id, over the scene in slot 2
};

struct ScheduledEvent
{
    bool enabled = true;
    char name[name_size] = "";
    Repeat repeat = Repeat::once;
    int16_t year = 2026;
    uint8_t month = 12;
    uint8_t day = 31;
    uint8_t days = every_day;
    uint32_t time_s = 23 * 3600 + 59 * 60;   // local time of day it starts
    EventAction action = EventAction::show;
    char target[target_size] = "";
    uint32_t duration_s = 600;               // then back to before; 0 = it stays
};

struct Schedule
{
    uint8_t timer_count = 0;
    TimerRule timers[max_timers];
    uint8_t event_count = 0;
    ScheduledEvent events[max_scheduled_events];
};

// What the timers say at local second t (seconds since 1970 in local time):
// 1 on, 0 off, -1 no timer enabled (the lights are left alone).
int timer_level(const Schedule &schedule, int64_t local_s);
// Whether an event happens on this local day (days since 1970).
bool event_on_day(const ScheduledEvent &event, int64_t day);
// Checked and tidied for storing: in range, names terminated. False if it
// can't be (e.g. a time past the end of the day).
bool valid_timer(const TimerRule &rule);
bool valid_event(ScheduledEvent &event);

// {"rev":N,"timers":[{"on":1,"d":127,"a":on_s,"b":off_s}...],"events":[{"on":1,
// "n":name,"r":repeat,"y":..,"m":..,"day":..,"d":days,"t":time_s,"w":action,
// "x":target,"len":duration_s}...]}. Returns the length written.
size_t describe_schedule(const Schedule &schedule, uint32_t revision, char *out, size_t cap);

}  // namespace neotree
