#include <cmath>
#include <vector>

#include "doctest/doctest.h"
#include "neotree/engine.hpp"

using namespace neotree;

namespace {

LedGeometry geometry;
Engine engine;

// LEDs up a column, 0..2000mm, radius 300 - the world is floor 0, ceiling 2000.
void setup(uint32_t tick_hz = 100)
{
    geometry.reset(21);
    for (uint16_t i = 0; i <= 20; i++)
    {
        geometry.set(i, led_point_from_cylindrical(100.0f * i, 300.0f, 30.0f * i));
    }
    geometry.finalize();
    EngineConfig config;
    config.tick_hz = tick_hz;
    config.max_ticks_per_advance = 0;
    engine.init(geometry, config);
}

Entity ball(uint8_t group, Vec3 pos, Vec3 vel, float radius = 50.0f)
{
    Entity e;
    e.slot = 1;
    e.size = radius;
    e.group = group;
    e.pos = pos;
    e.vel = vel;
    e.gravity_scale = 0.0f;
    e.restitution = 1.0f;
    return e;
}

// A tiny marker template in slot 1 (index returned), to count rule firings.
int marker_template()
{
    Entity m;
    m.size = 1.0f;
    m.gravity_scale = 0.0f;
    m.group = 9;
    return engine.behavior().add_template(1, m);
}

Action spawn_marker(int t)
{
    Action a;
    a.type = ActionType::spawn;
    a.index = static_cast<uint8_t>(t);
    a.place = Place::fixed;
    a.pos = {0.0f, 0.0f, 1000.0f};
    return a;
}

uint16_t markers() { return engine.behavior().count(1, 9); }

}  // namespace

TEST_CASE("behavior: bounce swaps equal-mass head-on velocities; one event per contact")
{
    setup(1000);
    Behavior &b = engine.behavior();
    b.set_response(1, 0, 0, Response::bounce);
    Rule r;
    r.trigger = Trigger::collision;
    r.group_a = 0;
    r.group_b = 0;
    r.then(spawn_marker(marker_template()));
    b.add_rule(1, r);

    Handle a = engine.spawn(ball(0, {-200.0f, 0.0f, 1000.0f}, {1000.0f, 0.0f, 0.0f}));
    Handle c = engine.spawn(ball(0, {200.0f, 0.0f, 1000.0f}, {-1000.0f, 0.0f, 0.0f}));
    engine.advance(300'000);
    CHECK(engine.entity(a)->vel.x == doctest::Approx(-1000.0f));
    CHECK(engine.entity(c)->vel.x == doctest::Approx(1000.0f));
    CHECK(markers() == 1);   // touched over several ticks, but one collision event
}

TEST_CASE("behavior: overlap passes through with begin and end events")
{
    setup(1000);
    Behavior &b = engine.behavior();
    b.set_response(1, 0, 1, Response::overlap);
    Rule begin;
    begin.trigger = Trigger::overlap_begin;
    begin.group_a = 0;
    begin.group_b = 1;
    Action red;
    red.type = ActionType::set_color;
    red.target = Target::a;
    red.color_from = ColorFrom::fixed;
    red.color = {1.0f, 0.0f, 0.0f};
    begin.then(red);
    b.add_rule(1, begin);
    Rule end = begin;
    end.trigger = Trigger::overlap_end;
    end.actions[0].color = {0.0f, 1.0f, 0.0f};
    b.add_rule(1, end);

    Handle a = engine.spawn(ball(0, {-200.0f, 0.0f, 1000.0f}, {1000.0f, 0.0f, 0.0f}));
    Handle c = engine.spawn(ball(1, {200.0f, 0.0f, 1000.0f}, {-1000.0f, 0.0f, 0.0f}));
    engine.advance(200'000);   // overlapping now
    CHECK(engine.entity(a)->color.r == 1.0f);
    CHECK(engine.entity(a)->vel.x == 1000.0f);   // unaffected
    engine.advance(300'000);   // through and apart
    CHECK(engine.entity(a)->color.g == 1.0f);
    CHECK(engine.entity(a)->pos.x > engine.entity(c)->pos.x);
}

TEST_CASE("behavior: destroy_a removes only the first group's entity; the rule's side follows the filter")
{
    setup(1000);
    Behavior &b = engine.behavior();
    b.set_response(1, 2, 3, Response::destroy_a);
    Handle g3 = engine.spawn(ball(3, {-200.0f, 0.0f, 1000.0f}, {1000.0f, 0.0f, 0.0f}));
    Handle g2 = engine.spawn(ball(2, {200.0f, 0.0f, 1000.0f}, {-1000.0f, 0.0f, 0.0f}));
    engine.advance(300'000);
    CHECK(engine.entity(g2) == nullptr);   // group 2 is the "a" side of (2, 3)
    CHECK(engine.entity(g3) != nullptr);
}

TEST_CASE("behavior: stick shares momentum")
{
    setup(1000);
    engine.behavior().set_response(1, 0, 0, Response::stick);
    Entity heavy = ball(0, {-200.0f, 0.0f, 1000.0f}, {1000.0f, 0.0f, 0.0f});
    heavy.mass = 3.0f;
    Handle a = engine.spawn(heavy);
    Handle c = engine.spawn(ball(0, {200.0f, 0.0f, 1000.0f}, {-1000.0f, 0.0f, 0.0f}));
    engine.advance(300'000);
    CHECK(engine.entity(a)->vel.x == doctest::Approx(500.0f));
    CHECK(engine.entity(c)->vel.x == doctest::Approx(500.0f));
}

TEST_CASE("behavior: events from actions wait a tick - a self-spawning chain grows one step per tick")
{
    setup(100);
    Behavior &b = engine.behavior();
    int t = marker_template();
    Rule chain;
    chain.trigger = Trigger::spawned;
    chain.group_a = 9;
    chain.then(spawn_marker(t));
    b.add_rule(1, chain);
    b.spawn(engine, 1, *b.template_at(1, static_cast<uint8_t>(t)));   // generation 0
    for (int tick = 1; tick <= 5; tick++)
    {
        engine.run_ticks(1);
        CHECK(markers() == tick + 1);
    }
}

TEST_CASE("behavior: the slot quota bounds spawning and counts refusals")
{
    setup(100);
    Behavior &b = engine.behavior();
    int t = marker_template();
    b.set_quota(1, 10);
    Rule chain;
    chain.trigger = Trigger::spawned;
    chain.group_a = 9;
    Action two = spawn_marker(t);
    two.count = 2;
    chain.then(two);
    b.add_rule(1, chain);
    b.spawn(engine, 1, *b.template_at(1, static_cast<uint8_t>(t)));
    engine.run_ticks(50);
    CHECK(markers() == 10);
    CHECK(b.stats().spawns_over_quota > 0);
}

TEST_CASE("behavior: the per-tick action budget is enforced and counted")
{
    setup(100);
    Behavior &b = engine.behavior();
    int t = marker_template();
    b.set_quota(1, 250);
    Rule on_input;
    on_input.trigger = Trigger::input;
    on_input.id = 7;
    for (int k = 0; k < max_actions_per_rule; k++)
    {
        on_input.then(spawn_marker(t));
    }
    b.add_rule(1, on_input);
    for (int k = 0; k < 40; k++)   // 40 events x 4 actions > the budget
    {
        engine.input(1, 7);
    }
    engine.run_ticks(1);
    CHECK(markers() == actions_per_tick);
    CHECK(b.stats().actions_dropped == 40 * max_actions_per_rule - actions_per_tick);
}

TEST_CASE("behavior: timers - delay, period, one-shot")
{
    setup(100);
    Behavior &b = engine.behavior();
    int t = marker_template();
    Rule every;
    every.trigger = Trigger::timer;
    every.delay_s = 0.5f;
    every.period_s = 0.25f;
    every.then(spawn_marker(t));
    b.add_rule(1, every);
    engine.advance(1'000'000);   // fires at 0.5, 0.75, 1.0
    CHECK(markers() == 3);

    setup(100);
    t = marker_template();
    Rule once;
    once.trigger = Trigger::timer;
    once.delay_s = 0.1f;
    once.then(spawn_marker(t));
    engine.behavior().add_rule(1, once);
    engine.advance(2'000'000);
    CHECK(markers() == 1);
}

TEST_CASE("behavior: count_below fires once when a group runs out")
{
    setup(100);
    Behavior &b = engine.behavior();
    int t = marker_template();
    Rule empty;
    empty.trigger = Trigger::count_below;
    empty.group_a = 4;
    empty.threshold = 1;
    empty.then(spawn_marker(t));
    b.add_rule(1, empty);
    engine.run_ticks(3);
    CHECK(markers() == 0);   // never had any: not armed yet

    Entity short_lived = ball(4, {0.0f, 0.0f, 500.0f}, {});
    short_lived.lifetime_s = 0.1f;
    engine.spawn(short_lived);
    engine.spawn(short_lived);
    engine.advance(1'000'000);
    CHECK(b.count(1, 4) == 0);
    CHECK(markers() == 1);
}

TEST_CASE("behavior: signals cross slots; inputs reach their slot")
{
    setup(100);
    Behavior &b = engine.behavior();
    Entity m;
    m.size = 1.0f;
    m.gravity_scale = 0.0f;
    m.group = 9;
    int t2 = b.add_template(2, m);

    Rule send;
    send.trigger = Trigger::input;
    send.id = 3;
    Action sig;
    sig.type = ActionType::signal;
    sig.index = 42;
    send.then(sig);
    b.add_rule(1, send);

    Rule receive;
    receive.trigger = Trigger::signal;
    receive.id = 42;
    Action a;
    a.type = ActionType::spawn;
    a.index = static_cast<uint8_t>(t2);
    receive.then(a);
    b.add_rule(2, receive);

    engine.input(1, 3);
    engine.run_ticks(3);
    CHECK(b.count(2, 9) == 1);
}

TEST_CASE("behavior: emitters spawn at their rate inside their region")
{
    setup(100);
    Behavior &b = engine.behavior();
    Entity e;
    e.size = 5.0f;
    e.gravity_scale = 0.0f;
    e.group = 5;
    int t = b.add_template(1, e);
    Emitter em;
    em.template_index = static_cast<uint8_t>(t);
    em.rate = 10.0f;
    em.region = Region::band;
    em.min = {100.0f, 0.0f, 400.0f};
    em.max = {200.0f, 0.0f, 600.0f};
    b.add_emitter(1, em);
    engine.advance(1'000'000);
    CHECK(b.count(1, 5) == 10);
    const EntityPool &pool = engine.entities();
    for (uint16_t k = 0; k < pool.capacity; k++)
    {
        if (!pool.alive(k))
        {
            continue;
        }
        const Entity &n = pool.item(k);
        float r = std::sqrt(n.pos.x * n.pos.x + n.pos.y * n.pos.y);
        CHECK(r >= 99.9f);
        CHECK(r <= 200.1f);
        CHECK(n.pos.z >= 400.0f);
        CHECK(n.pos.z <= 600.0f);
    }
}

TEST_CASE("behavior: boundary triggers filter by which boundary; expired carries the entity's data")
{
    setup(100);
    Behavior &b = engine.behavior();
    int t = marker_template();
    Rule floor_hit;
    floor_hit.trigger = Trigger::boundary;
    floor_hit.id = hit_floor;
    floor_hit.group_a = 6;
    floor_hit.then(spawn_marker(t));
    b.add_rule(1, floor_hit);
    Entity faller = ball(6, {0.0f, 0.0f, 1900.0f}, {0.0f, 0.0f, 3000.0f});
    faller.ceiling = Bound::bounce;   // hits the ceiling first: doesn't count
    faller.floor = Bound::destroy;
    engine.spawn(faller);
    engine.advance(300'000);
    CHECK(markers() == 0);
    engine.advance(1'000'000);
    CHECK(markers() == 1);

    setup(100);
    Behavior &b2 = engine.behavior();
    Entity spark;
    spark.size = 1.0f;
    spark.gravity_scale = 0.0f;
    spark.group = 9;
    int st = b2.add_template(1, spark);
    Rule burst;
    burst.trigger = Trigger::expired;
    burst.group_a = 0;
    Action at_a;
    at_a.type = ActionType::spawn;
    at_a.index = static_cast<uint8_t>(st);
    at_a.place = Place::a;
    at_a.color_from = ColorFrom::a;
    burst.then(at_a);
    b2.add_rule(1, burst);
    Entity rocket = ball(0, {0.0f, 0.0f, 500.0f}, {0.0f, 0.0f, 1000.0f});
    rocket.lifetime_s = 0.5f;
    rocket.color = {0.2f, 0.4f, 0.6f};
    engine.spawn(rocket);
    engine.advance(700'000);
    const EntityPool &pool = engine.entities();
    int found = 0;
    for (uint16_t k = 0; k < pool.capacity; k++)
    {
        if (pool.alive(k) && pool.item(k).group == 9)
        {
            found++;
            CHECK(pool.item(k).pos.z == doctest::Approx(1000.0f).epsilon(0.03));   // where the rocket died
            CHECK(pool.item(k).color.b == doctest::Approx(0.6f));
        }
    }
    CHECK(found == 1);
}

TEST_CASE("modes: the collision chain reaction stays within its quota over a long run")
{
    setup(120);
    engine.director().set_slot(engine, 1, SlotSpec::of("chain"), Transition::cut);
    for (int s = 0; s < 600; s++)   // 10 minutes of simulated time
    {
        engine.advance(1'000'000);
        REQUIRE(engine.behavior().slot_count(1) <= 40);
    }
    CHECK(engine.behavior().stats().peak_entities <= 40);
    CHECK(engine.behavior().stats().rule_fires > 0);
}

TEST_CASE("modes: fireworks burst into sparks")
{
    setup(120);
    engine.director().set_slot(engine, 1, SlotSpec::of("fireworks"), Transition::cut);
    engine.advance(5'000'000);
    CHECK(engine.behavior().stats().rule_fires > 0);
    CHECK(engine.behavior().stats().peak_entities > 30);
}
