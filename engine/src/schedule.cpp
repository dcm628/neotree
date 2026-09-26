#include "neotree/schedule.hpp"

#include "json.hpp"
#include "neotree/civil_time.hpp"

namespace neotree {

namespace {

using detail::Json;

int64_t floor_div(int64_t a, int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }
int weekday_of(int64_t day) { return static_cast<int>(((day % 7) + 11) % 7); }   // 1970-01-01: Thursday

bool terminated(char *s, size_t size)
{
    for (size_t i = 0; i < size; i++)
    {
        if (s[i] == '\0')
        {
            return true;
        }
        if (static_cast<unsigned char>(s[i]) < 0x20)
        {
            return false;
        }
    }
    s[size - 1] = '\0';
    return true;
}

}  // namespace

int timer_level(const Schedule &schedule, int64_t local_s)
{
    const int64_t today = floor_div(local_s, 86400);
    bool any = false;
    for (uint8_t i = 0; i < schedule.timer_count; i++)
    {
        const TimerRule &r = schedule.timers[i];
        if (!r.enabled || (r.days & every_day) == 0)
        {
            continue;
        }
        any = true;
        // A window starting yesterday may still be open (overnight).
        for (int64_t day = today - 1; day <= today; day++)
        {
            if ((r.days >> weekday_of(day) & 1) == 0)
            {
                continue;
            }
            const int64_t start = day * 86400 + r.on_s;
            const int64_t end = day * 86400 + r.off_s + (r.off_s <= r.on_s ? 86400 : 0);
            if (local_s >= start && local_s < end)
            {
                return 1;
            }
        }
    }
    return any ? 0 : -1;
}

bool event_on_day(const ScheduledEvent &e, int64_t day)
{
    switch (e.repeat)
    {
    case Repeat::weekly:
        return (e.days >> weekday_of(day) & 1) != 0;
    case Repeat::yearly:
    case Repeat::once:
    {
        int32_t y;
        uint32_t m, d;
        civil_from_days(day, y, m, d);
        return m == e.month && d == e.day && (e.repeat == Repeat::yearly || y == e.year);
    }
    }
    return false;
}

bool valid_timer(const TimerRule &r)
{
    return r.on_s < 86400 && r.off_s < 86400 && (r.days & ~every_day) == 0;
}

bool valid_event(ScheduledEvent &e)
{
    return terminated(e.name, sizeof(e.name)) && terminated(e.target, sizeof(e.target)) &&
           e.repeat <= Repeat::weekly && e.action <= EventAction::mode && e.time_s < 86400 && e.month >= 1 &&
           e.month <= 12 && e.day >= 1 && e.day <= 31 && (e.days & ~every_day) == 0 && e.target[0] != '\0';
}

size_t describe_schedule(const Schedule &s, uint32_t revision, char *out, size_t cap)
{
    if (cap == 0)
    {
        return 0;
    }
    Json j{out, cap};
    j.raw("{\"rev\":%lu,\"timers\":[", static_cast<unsigned long>(revision));
    for (uint8_t i = 0; i < s.timer_count; i++)
    {
        const TimerRule &r = s.timers[i];
        j.raw("%s{\"on\":%d,\"d\":%u,\"a\":%lu,\"b\":%lu}", i ? "," : "", r.enabled ? 1 : 0,
              static_cast<unsigned>(r.days), static_cast<unsigned long>(r.on_s), static_cast<unsigned long>(r.off_s));
    }
    j.raw("],\"events\":[");
    for (uint8_t i = 0; i < s.event_count; i++)
    {
        const ScheduledEvent &e = s.events[i];
        j.raw("%s{\"on\":%d,\"n\":", i ? "," : "", e.enabled ? 1 : 0);
        j.str(e.name);
        j.raw(",\"r\":%u,\"y\":%d,\"m\":%u,\"day\":%u,\"d\":%u,\"t\":%lu,\"w\":%u,\"x\":",
              static_cast<unsigned>(e.repeat), static_cast<int>(e.year), static_cast<unsigned>(e.month),
              static_cast<unsigned>(e.day), static_cast<unsigned>(e.days), static_cast<unsigned long>(e.time_s),
              static_cast<unsigned>(e.action));
        j.str(e.target);
        j.raw(",\"len\":%lu}", static_cast<unsigned long>(e.duration_s));
    }
    j.raw("]}");
    return j.len;
}

}  // namespace neotree
