#include <cstring>
#include <string>

#include "doctest/doctest.h"
#include "neotree/civil_time.hpp"
#include "neotree/engine.hpp"
#include "neotree/schedule.hpp"

using namespace neotree;

namespace {

LedGeometry geometry;
Engine engine;
TimeZone la;

void setup()
{
    geometry.reset(21);
    for (uint16_t i = 0; i <= 20; i++)
    {
        geometry.set(i, led_point_from_cylindrical(100.0f * i, 300.0f, 30.0f * i));
    }
    geometry.finalize();
    EngineConfig config;
    config.tick_hz = 100;
    config.max_ticks_per_advance = 0;
    engine.init(geometry, config);
    parse_time_zone("PST8PDT,M3.2.0,M11.1.0", la);
}

Director &dir() { return engine.director(); }

// Unix ms of a Los Angeles wall-clock time (PDT in summer and autumn, PST in winter).
int64_t la_ms(int32_t y, uint32_t mo, uint32_t d, int h, int mi = 0, int s = 0)
{
    const int64_t local = days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s;
    const int64_t pst = (local + 8 * 3600) * 1000;
    return in_dst(pst / 1000 - 3600, la) ? pst - 3600 * 1000 : pst;
}

// Runs the clock from `from` for `seconds`, the engine alongside, in steps.
int64_t run(int64_t from, double seconds, int step_ms = 100)
{
    int64_t t = from;
    const int64_t end = from + static_cast<int64_t>(seconds * 1000);
    while (t < end)
    {
        dir().run_schedule(engine, t, la);
        engine.advance(static_cast<int64_t>(step_ms) * 1000);
        t += step_ms;
    }
    dir().run_schedule(engine, t, la);
    return t;
}

TimerRule rule(int on_h, int on_m, int off_h, int off_m, uint8_t days = every_day)
{
    TimerRule r;
    r.on_s = static_cast<uint32_t>(on_h * 3600 + on_m * 60);
    r.off_s = static_cast<uint32_t>(off_h * 3600 + off_m * 60);
    r.days = days;
    return r;
}

ScheduledEvent event(const char *name, Repeat repeat, uint32_t time_s, EventAction action, const char *target,
                     uint32_t duration_s)
{
    ScheduledEvent e;
    std::snprintf(e.name, sizeof(e.name), "%s", name);
    e.repeat = repeat;
    e.time_s = time_s;
    e.action = action;
    std::snprintf(e.target, sizeof(e.target), "%s", target);
    e.duration_s = duration_s;
    return e;
}

}  // namespace

TEST_CASE("schedule: the timer's windows - days, overnight, none")
{
    Schedule s;
    CHECK(timer_level(s, 0) == -1);
    s.timers[0] = rule(17, 0, 23, 0, 0b0111110);   // weekdays
    s.timer_count = 1;
    const int64_t fri = days_from_civil(2026, 9, 25) * 86400;   // a Friday
    CHECK(timer_level(s, fri + 16 * 3600 + 3599) == 0);
    CHECK(timer_level(s, fri + 17 * 3600) == 1);
    CHECK(timer_level(s, fri + 23 * 3600 - 1) == 1);
    CHECK(timer_level(s, fri + 23 * 3600) == 0);
    CHECK(timer_level(s, fri + 86400 + 18 * 3600) == 0);   // Saturday: not on
    // On at 20:00, off at 01:00 - Friday only: still on early Saturday.
    s.timers[0] = rule(20, 0, 1, 0, 1 << 5);
    CHECK(timer_level(s, fri + 86400 + 30 * 60) == 1);
    CHECK(timer_level(s, fri + 86400 + 3600) == 0);
    CHECK(timer_level(s, fri + 30 * 60) == 0);            // Friday early: Thursday had none
    s.timers[0].enabled = false;
    CHECK(timer_level(s, fri + 21 * 3600) == -1);
}

TEST_CASE("schedule: which days an event happens")
{
    ScheduledEvent e;
    e.repeat = Repeat::once;
    e.year = 2026;
    e.month = 12;
    e.day = 31;
    CHECK(event_on_day(e, days_from_civil(2026, 12, 31)));
    CHECK_FALSE(event_on_day(e, days_from_civil(2027, 12, 31)));
    e.repeat = Repeat::yearly;
    CHECK(event_on_day(e, days_from_civil(2027, 12, 31)));
    CHECK_FALSE(event_on_day(e, days_from_civil(2027, 12, 30)));
    e.repeat = Repeat::weekly;
    e.days = 1 << 0;   // Sundays
    CHECK(event_on_day(e, days_from_civil(2026, 9, 27)));
    CHECK_FALSE(event_on_day(e, days_from_civil(2026, 9, 26)));
}

TEST_CASE("schedule: lights on and off like a plug-in timer; by hand lasts until the next change")
{
    setup();
    REQUIRE(dir().library().set_timer(0, rule(17, 0, 23, 0)));
    int64_t t = run(la_ms(2026, 9, 25, 16, 59, 50), 5);
    CHECK_FALSE(dir().lights());            // off: before 17:00, applied at once
    CHECK(dir().timer_state() == 0);
    t = run(t, 10);                         // past 17:00
    CHECK(dir().lights());
    CHECK_FALSE(engine.master().output_enabled == false);
    dir().set_lights(engine, false);        // off by hand at 17:00
    t = run(t, 60);
    CHECK_FALSE(dir().lights());            // stays off: no change due
    t = run(la_ms(2026, 9, 25, 22, 59, 55), 10);
    CHECK_FALSE(dir().lights());
    dir().set_lights(engine, true);         // on by hand after 23:00
    t = run(t, 120);
    CHECK(dir().lights());                  // stays on until tomorrow's 17:00
    t = run(la_ms(2026, 9, 26, 22, 59, 58), 4);
    CHECK_FALSE(dir().lights());            // and tomorrow's 23:00 turns it off

    // Editing the schedule applies it now.
    dir().set_lights(engine, true);
    REQUIRE(dir().library().set_timer(0, rule(6, 0, 7, 0)));
    run(t, 1);
    CHECK_FALSE(dir().lights());
    // No timer: the lights are left alone.
    REQUIRE(dir().library().delete_timer(0));
    dir().set_lights(engine, true);
    run(t + 2000, 1);
    CHECK(dir().lights());
    CHECK(dir().timer_state() == -1);
}

TEST_CASE("schedule: New Year's Eve - a show at 23:59:00 for two minutes, then back to before")
{
    setup();
    REQUIRE(dir().library().set_timer(0, rule(17, 0, 23, 0)));
    ScheduledEvent nye = event("New Year", Repeat::yearly, 23 * 3600 + 59 * 60, EventAction::show, "Holiday evening", 120);
    nye.month = 12;
    nye.day = 31;
    REQUIRE(dir().library().set_event(0, nye));
    dir().apply_scene(engine, dir().library().preset(dir().library().find_preset("Snow on rainbow")), Transition::cut);

    int64_t t = run(la_ms(2026, 12, 31, 23, 58, 50), 9.85);   // to 23:58:59.9
    CHECK_FALSE(dir().lights());            // the timer turned them off at 23:00
    CHECK(dir().running_event() == -1);
    t = run(t, 0.1);                        // 23:59:00
    CHECK(dir().running_event() == 0);
    CHECK(dir().lights());                  // on for the event
    CHECK(dir().show_status().playing);
    CHECK(std::string(dir().show_status().name) == "Holiday evening");
    static char json[4096];
    dir().describe_state(json, sizeof(json));
    CHECK(std::string(json).find("\"event\":{\"n\":\"New Year\",\"i\":0,\"left\":1") != std::string::npos);

    t = run(t, 119.8);
    CHECK(dir().running_event() == 0);
    t = run(t, 0.4);                        // 00:01:00: over
    CHECK(dir().running_event() == -1);
    CHECK_FALSE(dir().show_status().playing);
    CHECK_FALSE(dir().lights());            // as the timer says
    run(t, 3);                              // the scene fades back
    CHECK(dir().slot(1).spec.mode == find_mode("snow"));

    // Next year too; not again the same night.
    t = run(la_ms(2027, 12, 31, 23, 58, 59), 2);
    CHECK(dir().running_event() == 0);
}

TEST_CASE("schedule: events - once, a mode, one that stays, a jump in the clock, a DST gap, a missing target")
{
    setup();
    ScheduledEvent e = event("Snow at 6", Repeat::once, 18 * 3600, EventAction::mode, "snow", 60);
    e.year = 2026;
    e.month = 10;
    e.day = 3;
    REQUIRE(dir().library().set_event(0, e));
    run(la_ms(2026, 10, 3, 17, 59, 59), 2);
    CHECK(dir().running_event() == 0);
    CHECK(dir().slot(1).spec.mode == find_mode("snow"));
    dir().end_event(engine);                 // the app's "stop"
    CHECK(dir().running_event() == -1);

    // It stays (length 0): nothing to go back to.
    REQUIRE(dir().library().set_event(0, event("Evening", Repeat::weekly, 19 * 3600, EventAction::preset,
                                                "Snow on rainbow", 0)));
    run(la_ms(2026, 10, 3, 18, 59, 59), 2);
    CHECK(dir().running_event() == -1);
    CHECK(dir().slot(1).spec.mode == find_mode("snow"));

    // A clock that jumps over an event's moment doesn't fire it.
    dir().apply_scene(engine, dir().library().preset(0), Transition::cut);
    dir().run_schedule(engine, la_ms(2026, 10, 4, 18, 0), la);
    dir().run_schedule(engine, la_ms(2026, 10, 4, 19, 30), la);
    CHECK(dir().slot(1).state == SlotState::empty);

    // Spring forward: 02:30 never happens - the event fires at the jump.
    REQUIRE(dir().library().set_event(0, event("Night", Repeat::weekly, 2 * 3600 + 30 * 60, EventAction::mode, "snow", 30)));
    run(la_ms(2027, 3, 14, 1, 59, 58), 4);
    CHECK(dir().running_event() == 0);
    dir().end_event(engine);

    // A target that's gone: nothing happens.
    REQUIRE(dir().library().set_event(0, event("Gone", Repeat::weekly, 20 * 3600, EventAction::show, "No such show", 30)));
    run(la_ms(2026, 10, 5, 19, 59, 59), 2);
    CHECK(dir().running_event() == -1);
    CHECK(dir().start_event(engine, 1) == false);
}

TEST_CASE("schedule: stored with the library; bad entries refused; version 1 still loads")
{
    setup();
    Library &lib = dir().library();
    REQUIRE(lib.set_timer(0, rule(17, 0, 23, 0, 0b0111110)));
    REQUIRE(lib.set_timer(1, rule(20, 0, 1, 30)));
    ScheduledEvent e = event("Countdown to 2027", Repeat::once, 86340, EventAction::mode, "fx:A long effect name", 900);
    REQUIRE(lib.set_event(0, e));
    CHECK_FALSE(lib.set_timer(3, rule(1, 0, 2, 0)));          // past the end
    CHECK_FALSE(lib.set_timer(2, rule(25, 0, 2, 0)));
    ScheduledEvent bad = e;
    bad.month = 13;
    CHECK_FALSE(lib.set_event(1, bad));

    static uint8_t data[16384];
    const size_t n = lib.save(data, sizeof(data));
    REQUIRE(n > 0);
    lib.reset();
    CHECK(lib.schedule().timer_count == 0);
    REQUIRE(lib.load(data, n));
    CHECK(lib.schedule().timer_count == 2);
    CHECK(lib.schedule().timers[1].off_s == 5400);
    CHECK(lib.schedule().timers[0].days == 0b0111110);
    REQUIRE(lib.schedule().event_count == 1);
    CHECK(std::string(lib.schedule().events[0].target) == "fx:A long effect name");
    CHECK(lib.schedule().events[0].duration_s == 900);

    static char json[2048];
    lib.describe_schedule(json, sizeof(json));
    CHECK(std::string(json).find("\"timers\":[{\"on\":1,\"d\":62,\"a\":61200,\"b\":82800}") != std::string::npos);
    CHECK(std::string(json).find("\"n\":\"Countdown to 2027\",\"r\":0,\"y\":2026,\"m\":12,\"day\":31") != std::string::npos);

    // Version 1 (no schedule): the same without it.
    lib.delete_timer(1);
    lib.delete_timer(0);
    lib.delete_event(0);
    const size_t n1 = lib.save(data, sizeof(data));
    REQUIRE(n1 >= 2);
    data[0] = 1;
    REQUIRE(lib.load(data, n1 - 2));   // less the two zero counts
    CHECK(lib.schedule().timer_count == 0);
}
