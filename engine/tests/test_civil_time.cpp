#include <cstring>
#include <string>

#include "doctest/doctest.h"
#include "neotree/civil_time.hpp"

using namespace neotree;

namespace {

int64_t utc(int32_t y, uint32_t mo, uint32_t d, int h, int mi = 0, int s = 0)
{
    return days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s;
}

std::string local(int64_t t, const TimeZone &z)
{
    char buf[40];
    format_civil(local_time(t, z), buf, sizeof(buf));
    return buf;
}

}  // namespace

TEST_CASE("civil time: dates both ways, across eras and leap days")
{
    CHECK(days_from_civil(1970, 1, 1) == 0);
    CHECK(days_from_civil(2000, 3, 1) == 11017);
    CHECK(days_from_civil(1969, 12, 31) == -1);
    for (int64_t d = -800000; d <= 800000; d += 997)
    {
        int32_t y;
        uint32_t m, day;
        civil_from_days(d, y, m, day);
        CHECK(days_from_civil(y, m, day) == d);
    }
    const CivilTime t = civil_from_unix(utc(2024, 2, 29, 23, 59, 58));
    CHECK(t.year == 2024);
    CHECK(t.month == 2);
    CHECK(t.day == 29);
    CHECK(t.second == 58);
    CHECK(t.weekday == 4);   // a Thursday
    CHECK(t.yday == 59);
    CHECK(civil_from_unix(0).weekday == 4);
    CHECK(civil_from_unix(-86400).weekday == 3);
}

TEST_CASE("civil time: Los Angeles - DST from the second Sunday of March to the first of November")
{
    TimeZone la;
    REQUIRE(parse_time_zone("PST8PDT,M3.2.0,M11.1.0", la));
    CHECK(la.std_offset_s == -8 * 3600);
    CHECK(la.dst_offset_s == -7 * 3600);
    CHECK(local(utc(2026, 9, 25, 16, 56, 47), la) == "2026-09-25 09:56:47 PDT");
    CHECK(local(utc(2026, 12, 25, 20), la) == "2026-12-25 12:00:00 PST");
    // 2026-03-08 02:00 PST = 10:00 UTC: the clock jumps to 03:00 PDT.
    CHECK(local(utc(2026, 3, 8, 9, 59, 59), la) == "2026-03-08 01:59:59 PST");
    CHECK(local(utc(2026, 3, 8, 10), la) == "2026-03-08 03:00:00 PDT");
    // 2026-11-01 02:00 PDT = 09:00 UTC: back to 01:00 PST.
    CHECK(local(utc(2026, 11, 1, 8, 59, 59), la) == "2026-11-01 01:59:59 PDT");
    CHECK(local(utc(2026, 11, 1, 9), la) == "2026-11-01 01:00:00 PST");
    // No rules given: the US ones.
    TimeZone us;
    REQUIRE(parse_time_zone("EST5EDT", us));
    CHECK(local(utc(2026, 7, 4, 16), us) == "2026-07-04 12:00:00 EDT");
}

TEST_CASE("civil time: London, the last Sunday, with its own change times")
{
    TimeZone uk;
    REQUIRE(parse_time_zone("GMT0BST,M3.5.0/1,M10.5.0", uk));
    CHECK(local(utc(2026, 3, 29, 0, 59), uk) == "2026-03-29 00:59:00 GMT");
    CHECK(local(utc(2026, 3, 29, 1), uk) == "2026-03-29 02:00:00 BST");
    CHECK(local(utc(2026, 10, 25, 0, 59), uk) == "2026-10-25 01:59:00 BST");
    CHECK(local(utc(2026, 10, 25, 1), uk) == "2026-10-25 01:00:00 GMT");
}

TEST_CASE("civil time: Sydney - DST across the new year; India - half an hour, no DST")
{
    TimeZone syd;
    REQUIRE(parse_time_zone("AEST-10AEDT,M10.1.0,M4.1.0/3", syd));
    CHECK(local(utc(2026, 1, 15, 0), syd) == "2026-01-15 11:00:00 AEDT");
    CHECK(local(utc(2026, 4, 4, 15, 59), syd) == "2026-04-05 02:59:00 AEDT");
    CHECK(local(utc(2026, 4, 4, 16), syd) == "2026-04-05 02:00:00 AEST");
    CHECK(local(utc(2026, 7, 1, 0), syd) == "2026-07-01 10:00:00 AEST");
    CHECK(local(utc(2026, 10, 3, 16), syd) == "2026-10-04 03:00:00 AEDT");
    CHECK(local(utc(2026, 12, 31, 13), syd) == "2027-01-01 00:00:00 AEDT");

    TimeZone india;
    REQUIRE(parse_time_zone("IST-5:30", india));
    CHECK_FALSE(india.has_dst);
    CHECK(local(utc(2026, 6, 1, 0), india) == "2026-06-01 05:30:00 IST");
    TimeZone quoted;
    REQUIRE(parse_time_zone("<+0545>-5:45", quoted));
    CHECK(local(utc(2026, 6, 1, 0), quoted) == "2026-06-01 05:45:00 +0545");
}

TEST_CASE("civil time: bad rules are refused and leave UTC")
{
    TimeZone z;
    for (const char *bad : {"", "P8", "PST", "PST8PDT,M3.2.0", "PST8PDT,M13.2.0,M11.1.0", "PST8PDT,M3.6.0,M11.1.0",
                            "PST8PDT,M3.2.7,M11.1.0", "PST8x", "<PS T>8", "PST999", "PST8PDT,J0,J365"})
    {
        CAPTURE(bad);
        CHECK_FALSE(parse_time_zone(bad, z));
        CHECK(std::strcmp(z.std_name, "UTC") == 0);
        CHECK(z.std_offset_s == 0);
    }
    CHECK(parse_time_zone("UTC0", z));
    CHECK(parse_time_zone("XYZ-3XYW,J60/0,300/25", z));
}
