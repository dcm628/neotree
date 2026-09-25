#include <algorithm>
#include <cmath>

#include "doctest/doctest.h"
#include "neotree/direct.hpp"
#include "neotree/engine.hpp"

using namespace neotree;

namespace {

LedGeometry geometry;
Engine engine;

// A cone of 600 LEDs like the tree: 2 m tall, 600 mm radius at the bottom.
void setup()
{
    geometry.reset(600);
    for (uint16_t i = 0; i < 600; i++)
    {
        const float z = 3.3f * i;
        geometry.set(i, led_point_from_cylindrical(z, 600.0f - 0.27f * z, 137.5f * i));
    }
    geometry.finalize();
    EngineConfig config;
    config.tick_hz = 100;
    config.max_ticks_per_advance = 0;
    engine.init(geometry, config);
}

Director &dir() { return engine.director(); }
void seconds(float s) { engine.advance(static_cast<int64_t>(s * 1e6f)); }

void start_play(uint8_t slot, float fade = 4.0f)
{
    SlotSpec s = SlotSpec::of("play").set("fade", fade);
    dir().set_slot(engine, slot, s, Transition::cut);
}

uint16_t owned(uint8_t owner, DirectKind kind)
{
    uint16_t n = 0;
    const EntityPool &pool = engine.entities();
    for (uint16_t k = 0; k < pool.capacity; k++)
    {
        n += pool.alive(k) && pool.item(k).owner == owner && pool.item(k).direct_kind == static_cast<uint8_t>(kind);
    }
    return n;
}

const Entity *find(uint8_t owner, uint8_t id)
{
    const EntityPool &pool = engine.entities();
    for (uint16_t k = 0; k < pool.capacity; k++)
    {
        if (pool.alive(k) && pool.item(k).owner == owner && pool.item(k).direct_id == id)
        {
            return &pool.item(k);
        }
    }
    return nullptr;
}

// LEDs within r of p with some alpha in the layer.
int painted_near(std::span<const Rgba8> px, Vec3 p, float r)
{
    int n = 0;
    for (uint16_t i : geometry.z_order())
    {
        const Vec3 q = geometry.position(i);
        const float d = std::sqrt((q.x - p.x) * (q.x - p.x) + (q.y - p.y) * (q.y - p.y) + (q.z - p.z) * (q.z - p.z));
        n += d < r && px[i].a > 0;
    }
    return n;
}

int painted(std::span<const Rgba8> px)
{
    int n = 0;
    for (uint16_t i : geometry.z_order())
    {
        n += px[i].a > 0;
    }
    return n;
}

const Rgb red{1.0f, 0.0f, 0.0f};

}  // namespace

TEST_CASE("direct: balls only spawn where a mode offers direct control; they fall and stay in the tree")
{
    setup();
    CHECK(!direct_spawn(engine, 1, 1, 0, DirectKind::ball, {0, 300, 1500}, {}, red, 0));   // nothing in slot 1
    dir().set_slot(engine, 1, SlotSpec::of("snow"), Transition::cut);
    CHECK(!direct_spawn(engine, 1, 1, 0, DirectKind::ball, {0, 300, 1500}, {}, red, 0));   // snow doesn't offer it
    start_play(1);
    CHECK(!direct_spawn(engine, 1, 0, 0, DirectKind::ball, {0, 300, 1500}, {}, red, 0));   // owner 0 is the mode's
    REQUIRE(direct_spawn(engine, 1, 1, 0, DirectKind::ball, {0, 300, 1500}, {800, 0, 0}, red, 0));
    const Entity *ball = find(1, 0);
    REQUIRE(ball != nullptr);
    CHECK(ball->slot == 1);
    seconds(0.5f);
    CHECK(ball->pos.z < 1500.0f);   // falling
    seconds(5.0f);
    const World &w = engine.world();
    CHECK(ball->pos.z >= w.floor_z - 1.0f);
    CHECK(std::sqrt(ball->pos.x * ball->pos.x + ball->pos.y * ball->pos.y) <=
          geometry.envelope_radius(ball->pos.z) + w.outer_margin_mm + 1.0f);
    seconds(20.0f);
    CHECK(find(1, 0) == nullptr);   // lifetime over
}

TEST_CASE("direct: a phone's balls are capped (oldest go), ids replace, and release takes only its own")
{
    setup();
    start_play(2);
    for (uint8_t id = 0; id < max_direct_balls + 3; id++)
    {
        REQUIRE(direct_spawn(engine, 2, 3, id, DirectKind::ball, {0, 200, 1000}, {}, red, 0));
        seconds(0.05f);
    }
    CHECK(owned(3, DirectKind::ball) == max_direct_balls);
    CHECK(find(3, 0) == nullptr);   // the oldest went
    CHECK(find(3, max_direct_balls + 2) != nullptr);

    REQUIRE(direct_spawn(engine, 2, 3, max_direct_balls + 2, DirectKind::ball, {0, 0, 500}, {}, red, 40.0f));
    CHECK(owned(3, DirectKind::ball) == max_direct_balls);
    CHECK(find(3, max_direct_balls + 2)->size == 40.0f);

    REQUIRE(direct_spawn(engine, 2, 4, 0, DirectKind::ball, {0, 200, 1000}, {}, red, 0));
    CHECK(direct_release(engine, 3) == max_direct_balls);
    CHECK(owned(3, DirectKind::ball) == 0);
    CHECK(owned(4, DirectKind::ball) == 1);
    CHECK(direct_count(engine) == 1);
}

TEST_CASE("direct: a brush follows its samples, paints only with the pen down, and goes when they stop")
{
    setup();
    start_play(1, 0.0f + 30.0f);   // slow fade: nothing disappears during the test
    const auto trail = [] { return engine.scene().pixels(1, 0); };
    BrushSample s;
    s.color = red;
    s.radius_mm = 90.0f;
    s.pos = {0, 500, 300};
    s.pen_down = false;
    REQUIRE(direct_brush(engine, 1, 2, 0, s));
    const Entity *brush = find(2, 0);
    REQUIRE(brush != nullptr);
    CHECK(brush->kinematic);
    CHECK(painted(trail()) == 0);   // hovering: no paint

    // Pen down, moving up the side of the tree.
    for (int k = 0; k <= 20; k++)
    {
        s.pen_down = true;
        s.pos = {0, 500.0f - 5.0f * k, 300.0f + 40.0f * k};
        REQUIRE(direct_brush(engine, 1, 2, 0, s));
        seconds(1.0f / 60.0f);
    }
    // At the last sample, glided on by at most a tick or two at ~2400 mm/s.
    CHECK(brush->pos.z >= 1100.0f);
    CHECK(brush->pos.z < 1160.0f);
    CHECK(painted_near(trail(), {0, 450, 700}, 120.0f) > 0);   // the middle of the stroke
    const int after_stroke = painted(trail());
    CHECK(after_stroke > 0);

    // Pen up: it moves, but paints nothing more.
    s.pen_down = false;
    s.pos = {0, -500, 300};
    REQUIRE(direct_brush(engine, 1, 2, 0, s));
    CHECK(painted(trail()) == after_stroke);

    // Between samples it coasts to a stop; with none for its lease, it's gone.
    seconds(0.2f);
    CHECK(brush->vel.x == 0.0f);
    CHECK(brush->vel.y == 0.0f);
    seconds(1.0f);
    CHECK(find(2, 0) == nullptr);
}

TEST_CASE("direct: trails fade over the fade time; fade 0 paints the Colors canvas instead")
{
    setup();
    SceneSpec scene;
    scene.specs[0] = SlotSpec::of("canvas");
    scene.specs[1] = SlotSpec::of("play").set("fade", 2.0f);
    dir().apply_scene(engine, scene, Transition::cut);
    BrushSample s;
    s.color = red;
    s.radius_mm = 100.0f;
    s.pen_down = true;
    s.pos = {0, 400, 900};
    REQUIRE(direct_brush(engine, 1, 5, 1, s));
    const int fresh = painted(engine.scene().pixels(1, 0));
    CHECK(fresh > 0);
    seconds(1.0f);
    CHECK(painted(engine.scene().pixels(1, 0)) > 0);   // fading
    seconds(1.1f);
    CHECK(painted(engine.scene().pixels(1, 0)) == 0);  // gone after 2 s

    // Keep: onto the Canvas's paint layer, not the trail.
    ParamValue keep;
    keep.f = 0.0f;
    REQUIRE(dir().set_param(engine, 1, 0, keep));
    const int canvas_before = painted(engine.scene().pixels(0, 1));
    s.pos = {0, -400, 600};
    REQUIRE(direct_brush(engine, 1, 5, 1, s));
    CHECK(painted(engine.scene().pixels(1, 0)) == 0);
    CHECK(painted(engine.scene().pixels(0, 1)) > canvas_before);
    seconds(10.0f);
    CHECK(painted(engine.scene().pixels(0, 1)) > canvas_before);   // kept
}

TEST_CASE("direct: a brush bats a ball")
{
    setup();
    start_play(1);
    ParamValue none;
    none.f = 0.0f;
    REQUIRE(dir().set_param(engine, 1, 1, none));   // no gravity: the ball floats still
    REQUIRE(direct_spawn(engine, 1, 1, 7, DirectKind::ball, {0, 300, 1000}, {}, red, 60.0f));
    const Entity *ball = find(1, 7);
    REQUIRE(ball != nullptr);
    CHECK(ball->gravity_scale == 0.0f);
    // Swing the brush through the ball along +x.
    BrushSample s;
    s.color = red;
    s.radius_mm = 60.0f;
    float fastest = 0.0f;
    for (int k = 0; k <= 12; k++)
    {
        s.pos = {-400.0f + 60.0f * k, 300.0f, 1000.0f};
        REQUIRE(direct_brush(engine, 1, 2, 0, s));
        seconds(1.0f / 60.0f);
        fastest = std::max(fastest, ball->vel.x);
    }
    // Knocked along +x, faster than the brush (it then bounces around the tree).
    CHECK(fastest > 3000.0f);   // knocked along +x
}

namespace {

// Two balls from phone 1 flying at each other along x at 1 m.
void head_on(uint8_t slot)
{
    // Inside the tree (radius ~380 mm at 800 mm up), meeting at the trunk.
    REQUIRE(direct_spawn(engine, slot, 1, 0, DirectKind::ball, {-250, 0, 800}, {1500, 0, 0}, {1, 0, 0}, 60.0f));
    REQUIRE(direct_spawn(engine, slot, 1, 1, DirectKind::ball, {250, 0, 800}, {-1500, 0, 0}, {0, 0, 1}, 60.0f));
}

void set_play(uint8_t slot, uint8_t param, float value)
{
    ParamValue v;
    v.f = value;
    REQUIRE(dir().set_param(engine, slot, param, v));
}

uint16_t mode_owned()
{
    uint16_t n = 0;
    const EntityPool &pool = engine.entities();
    for (uint16_t k = 0; k < pool.capacity; k++)
    {
        n += pool.alive(k) && pool.item(k).owner == 0;
    }
    return n;
}

}  // namespace

TEST_CASE("play: when balls meet - bounce, pass through, burst into sparks, or mix colors")
{
    // Params: 1 gravity, 4 hit (0 bounce, 1 pass, 2 burst, 3 mix).
    setup();
    start_play(1);
    set_play(1, 1, 0.0f);
    head_on(1);
    seconds(0.25f);   // they meet at ~0.1 s; before they reach the far side
    CHECK(find(1, 0)->vel.x < 0.0f);   // bounced back
    CHECK(find(1, 1)->vel.x > 0.0f);

    setup();
    start_play(1);
    set_play(1, 1, 0.0f);
    set_play(1, 4, 1.0f);
    head_on(1);
    seconds(0.25f);
    CHECK(find(1, 0)->pos.x > 0.0f);   // passed straight through
    CHECK(find(1, 0)->vel.x > 1000.0f);

    setup();
    start_play(1);
    set_play(1, 1, 0.0f);
    set_play(1, 4, 2.0f);
    head_on(1);
    seconds(0.4f);
    CHECK(find(1, 0) == nullptr);   // both gone...
    CHECK(find(1, 1) == nullptr);
    CHECK(mode_owned() == 16);      // ...in 8 sparks each (the mode's, not the phone's)
    seconds(2.0f);
    CHECK(mode_owned() == 0);       // sparks burn out

    setup();
    start_play(1);
    set_play(1, 1, 0.0f);
    set_play(1, 4, 3.0f);
    head_on(1);
    seconds(0.4f);
    for (uint8_t id : {0, 1})
    {
        const Entity *ball = find(1, id);
        REQUIRE(ball != nullptr);
        CHECK(ball->color.r == doctest::Approx(0.5f));   // red and blue meet in the middle
        CHECK(ball->color.b == doctest::Approx(0.5f));
    }
}

TEST_CASE("play: comet tails paint where balls go; look and physics change balls in flight")
{
    setup();
    start_play(1, 30.0f);
    set_play(1, 1, 0.0f);
    REQUIRE(direct_spawn(engine, 1, 1, 0, DirectKind::ball, {-250, 250, 800}, {800, 0, 0}, red, 50.0f));
    seconds(0.3f);
    CHECK(painted(engine.scene().pixels(1, 0)) == 0);   // tails off: no trail
    set_play(1, 5, 1.0f);
    seconds(0.4f);
    CHECK(painted(engine.scene().pixels(1, 0)) > 0);
    CHECK(painted_near(engine.scene().pixels(1, 0), find(1, 0)->pos, 150.0f) > 0);

    // Look 2 = bubble, drag and life follow too - for the ball already flying.
    set_play(1, 3, 2.0f);
    set_play(1, 6, 1.5f);
    set_play(1, 7, 5.0f);
    const Entity *ball = find(1, 0);
    CHECK(ball->shape == Shape::shell);
    CHECK(ball->drag == 1.5f);
    CHECK(ball->lifetime_s == 5.0f);
    CHECK(collision_radius(*ball) > ball->size);   // bubbles still collide
    set_play(1, 3, 1.0f);
    CHECK(ball->shape == Shape::sphere);
    CHECK(ball->falloff == Falloff::glow);
}
