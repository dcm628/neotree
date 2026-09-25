#pragma once
// Civil (wall-clock) time: calendar dates from Unix time, and time zones
// with daylight saving from POSIX TZ rules - e.g. "PST8PDT,M3.2.0,M11.1.0",
// "GMT0BST,M3.5.0/1,M10.5.0", "<+0530>-5:30". No time zone database: the
// app sends the rule for its zone, and the tree keeps local time with it
// (DST changes included) on its own.

#include <cstddef>
#include <cstdint>

namespace neotree {

// When a DST change happens in a year: a day, and a local time of day.
struct TzRule
{
    enum class Kind : uint8_t
    {
        month_week_day,   // Mm.w.d: day d (0 = Sunday) of week w (1-4, 5 = the last) of month m
        julian_no_leap,   // Jn: day n (1-365), February 29th never counted
        julian,           // n: day n (0-365), February 29th counted
    };
    Kind kind = Kind::month_week_day;
    uint8_t month = 0;
    uint8_t week = 0;
    uint8_t weekday = 0;
    uint16_t day = 0;
    int32_t time_s = 2 * 3600;   // local time of day it happens at (may be negative or past 24 h)
};

struct TimeZone
{
    int32_t std_offset_s = 0;    // local standard time = UTC + this
    int32_t dst_offset_s = 0;
    bool has_dst = false;
    TzRule start{};              // DST starts (in standard time)
    TzRule end{};                // DST ends (in daylight time)
    char std_name[8] = "UTC";
    char dst_name[8] = "";
};

// Parses a POSIX TZ rule. Returns false (leaving out as UTC) if it isn't one.
bool parse_time_zone(const char *tz, TimeZone &out);

struct CivilTime
{
    int32_t year = 1970;
    uint8_t month = 1;       // 1-12
    uint8_t day = 1;         // 1-31
    uint8_t hour = 0;
    uint8_t minute = 0;
    uint8_t second = 0;
    uint8_t weekday = 4;     // 0 = Sunday
    uint16_t yday = 0;       // 0-365
    bool dst = false;
    int32_t offset_s = 0;    // local = UTC + this
    char zone[8] = "UTC";    // its abbreviation
};

// Days since 1970-01-01 of a date, and back (any year, proleptic Gregorian).
int64_t days_from_civil(int32_t year, uint32_t month, uint32_t day);
void civil_from_days(int64_t days, int32_t &year, uint32_t &month, uint32_t &day);

// The calendar date and time in UTC plus offset_s.
CivilTime civil_from_unix(int64_t unix_s, int32_t offset_s = 0);
// Local time in a zone.
CivilTime local_time(int64_t unix_s, const TimeZone &zone);
// Whether the zone is on daylight time at this instant.
bool in_dst(int64_t unix_s, const TimeZone &zone);

// "2026-09-25 09:56:47 PDT". Returns the length written.
size_t format_civil(const CivilTime &t, char *out, size_t cap);

}  // namespace neotree
