#include "neotree/civil_time.hpp"

#include <cstdio>
#include <cstring>

namespace neotree {

namespace {

bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

// A number of up to max_digits digits.
bool parse_number(const char *&p, int max_digits, int &out)
{
    if (!is_digit(*p))
    {
        return false;
    }
    out = 0;
    for (int n = 0; n < max_digits && is_digit(*p); n++)
    {
        out = out * 10 + (*p++ - '0');
    }
    return !is_digit(*p);
}

// A zone name: 3+ letters, or <...> of letters, digits, + and -.
bool parse_name(const char *&p, char (&out)[8])
{
    size_t n = 0;
    if (*p == '<')
    {
        p++;
        while (*p != '>' && *p != '\0')
        {
            if (!is_alpha(*p) && !is_digit(*p) && *p != '+' && *p != '-')
            {
                return false;
            }
            if (n + 1 < sizeof(out))
            {
                out[n++] = *p;
            }
            p++;
        }
        if (*p != '>')
        {
            return false;
        }
        p++;
    }
    else
    {
        while (is_alpha(*p))
        {
            if (n + 1 < sizeof(out))
            {
                out[n++] = *p;
            }
            p++;
        }
    }
    out[n] = '\0';
    return n >= 3;
}

// [+-]hh[:mm[:ss]], hours up to 167 (for rule times).
bool parse_hms(const char *&p, int32_t &out)
{
    int sign = 1;
    if (*p == '+' || *p == '-')
    {
        sign = *p == '-' ? -1 : 1;
        p++;
    }
    int h = 0, m = 0, s = 0;
    if (!parse_number(p, 3, h) || h > 167)
    {
        return false;
    }
    if (*p == ':')
    {
        p++;
        if (!parse_number(p, 2, m) || m > 59)
        {
            return false;
        }
        if (*p == ':')
        {
            p++;
            if (!parse_number(p, 2, s) || s > 59)
            {
                return false;
            }
        }
    }
    out = sign * (h * 3600 + m * 60 + s);
    return true;
}

bool parse_rule(const char *&p, TzRule &r)
{
    int a = 0, b = 0, c = 0;
    if (*p == 'M')
    {
        p++;
        if (!parse_number(p, 2, a) || *p++ != '.' || !parse_number(p, 1, b) || *p++ != '.' || !parse_number(p, 1, c) ||
            a < 1 || a > 12 || b < 1 || b > 5 || c > 6)
        {
            return false;
        }
        r.kind = TzRule::Kind::month_week_day;
        r.month = static_cast<uint8_t>(a);
        r.week = static_cast<uint8_t>(b);
        r.weekday = static_cast<uint8_t>(c);
    }
    else if (*p == 'J')
    {
        p++;
        if (!parse_number(p, 3, a) || a < 1 || a > 365)
        {
            return false;
        }
        r.kind = TzRule::Kind::julian_no_leap;
        r.day = static_cast<uint16_t>(a);
    }
    else
    {
        if (!parse_number(p, 3, a) || a > 365)
        {
            return false;
        }
        r.kind = TzRule::Kind::julian;
        r.day = static_cast<uint16_t>(a);
    }
    r.time_s = 2 * 3600;
    if (*p == '/')
    {
        p++;
        return parse_hms(p, r.time_s);
    }
    return true;
}

bool leap(int32_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

uint32_t days_in_month(int32_t y, uint32_t m)
{
    static const uint8_t days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && leap(y) ? 29 : days[m - 1];
}

int64_t floor_div(int64_t a, int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }

// Local seconds since 1970 (local midnight Jan 1st 1970 = 0) at which a rule
// happens in year y.
int64_t rule_local_s(const TzRule &r, int32_t y)
{
    int64_t days = 0;
    switch (r.kind)
    {
    case TzRule::Kind::month_week_day:
    {
        const int64_t first = days_from_civil(y, r.month, 1);
        const int first_weekday = static_cast<int>(((first % 7) + 11) % 7);   // 1970-01-01 was a Thursday
        uint32_t day = 1 + static_cast<uint32_t>((r.weekday - first_weekday + 7) % 7) + (r.week - 1u) * 7u;
        while (day > days_in_month(y, r.month))
        {
            day -= 7;   // week 5: the last one
        }
        days = first + day - 1;
        break;
    }
    case TzRule::Kind::julian_no_leap:
        days = days_from_civil(y, 1, 1) + r.day - 1 + (leap(y) && r.day >= 60 ? 1 : 0);
        break;
    case TzRule::Kind::julian:
        days = days_from_civil(y, 1, 1) + r.day;
        break;
    }
    return days * 86400 + r.time_s;
}

}  // namespace

bool parse_time_zone(const char *tz, TimeZone &out)
{
    out = TimeZone{};
    TimeZone z;
    const char *p = tz;
    if (p == nullptr || !parse_name(p, z.std_name))
    {
        return false;
    }
    int32_t west = 0;   // POSIX offsets are west of UTC
    if (!parse_hms(p, west))
    {
        return false;
    }
    z.std_offset_s = -west;
    if (*p != '\0')
    {
        if (!parse_name(p, z.dst_name))
        {
            return false;
        }
        z.has_dst = true;
        z.dst_offset_s = z.std_offset_s + 3600;
        if (*p != ',' && *p != '\0')
        {
            if (!parse_hms(p, west))
            {
                return false;
            }
            z.dst_offset_s = -west;
        }
        if (*p == '\0')
        {
            // No rules given: the US ones (POSIX leaves it to the system).
            z.start = TzRule{TzRule::Kind::month_week_day, 3, 2, 0, 0, 2 * 3600};
            z.end = TzRule{TzRule::Kind::month_week_day, 11, 1, 0, 0, 2 * 3600};
        }
        else if (*p++ != ',' || !parse_rule(p, z.start) || *p++ != ',' || !parse_rule(p, z.end))
        {
            return false;
        }
    }
    if (*p != '\0')
    {
        return false;
    }
    out = z;
    return true;
}

int64_t days_from_civil(int32_t year, uint32_t month, uint32_t day)
{
    // Howard Hinnant's algorithm.
    const int64_t y = static_cast<int64_t>(year) - (month <= 2 ? 1 : 0);
    const int64_t era = floor_div(y, 400);
    const int64_t yoe = y - era * 400;
    const int64_t mp = (month + 9) % 12;
    const int64_t doy = (153 * mp + 2) / 5 + day - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void civil_from_days(int64_t days, int32_t &year, uint32_t &month, uint32_t &day)
{
    const int64_t z = days + 719468;
    const int64_t era = floor_div(z, 146097);
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    day = static_cast<uint32_t>(doy - (153 * mp + 2) / 5 + 1);
    month = static_cast<uint32_t>(mp < 10 ? mp + 3 : mp - 9);
    year = static_cast<int32_t>(yoe + era * 400 + (month <= 2 ? 1 : 0));
}

CivilTime civil_from_unix(int64_t unix_s, int32_t offset_s)
{
    const int64_t local = unix_s + offset_s;
    const int64_t days = floor_div(local, 86400);
    const int64_t secs = local - days * 86400;
    CivilTime t;
    uint32_t month = 1, day = 1;
    civil_from_days(days, t.year, month, day);
    t.month = static_cast<uint8_t>(month);
    t.day = static_cast<uint8_t>(day);
    t.hour = static_cast<uint8_t>(secs / 3600);
    t.minute = static_cast<uint8_t>(secs / 60 % 60);
    t.second = static_cast<uint8_t>(secs % 60);
    t.weekday = static_cast<uint8_t>(((days % 7) + 11) % 7);
    t.yday = static_cast<uint16_t>(days - days_from_civil(t.year, 1, 1));
    t.offset_s = offset_s;
    return t;
}

bool in_dst(int64_t unix_s, const TimeZone &zone)
{
    if (!zone.has_dst)
    {
        return false;
    }
    // The year as standard time has it; each change at its local time, in
    // the offset in force just before it.
    const int32_t year = civil_from_unix(unix_s, zone.std_offset_s).year;
    const int64_t start = rule_local_s(zone.start, year) - zone.std_offset_s;
    const int64_t end = rule_local_s(zone.end, year) - zone.dst_offset_s;
    // Southern hemisphere: DST spans the new year (start after end).
    return start < end ? unix_s >= start && unix_s < end : !(unix_s >= end && unix_s < start);
}

CivilTime local_time(int64_t unix_s, const TimeZone &zone)
{
    const bool dst = in_dst(unix_s, zone);
    CivilTime t = civil_from_unix(unix_s, dst ? zone.dst_offset_s : zone.std_offset_s);
    t.dst = dst;
    std::memcpy(t.zone, dst ? zone.dst_name : zone.std_name, sizeof(t.zone));
    return t;
}

size_t format_civil(const CivilTime &t, char *out, size_t cap)
{
    if (cap == 0)
    {
        return 0;
    }
    const int n = std::snprintf(out, cap, "%04ld-%02u-%02u %02u:%02u:%02u %s", static_cast<long>(t.year),
                                static_cast<unsigned>(t.month), static_cast<unsigned>(t.day),
                                static_cast<unsigned>(t.hour), static_cast<unsigned>(t.minute),
                                static_cast<unsigned>(t.second), t.zone);
    return n < 0 ? 0 : (static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : cap - 1);
}

}  // namespace neotree
