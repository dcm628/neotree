#include <cmath>
#include <vector>

#include "doctest/doctest.h"
#include "neotree/engine.hpp"

using namespace neotree;

namespace {

LedGeometry geometry;
Engine engine;

// A column of LEDs every 100mm from z=0 to 2000 at radius 300, angle
// stepping 30 degrees - enough spread for shapes, envelope and culling.
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

float cover(const Entity &e, Vec3 p)
{
    float r = std::sqrt(p.x * p.x + p.y * p.y);
    float a = std::atan2(p.y, p.x);
    return entity_coverage(e, p, r, a < 0.0f ? a + two_pi : a);
}

Entity sphere(float radius, Falloff falloff = Falloff::hard)
{
    Entity e;
    e.shape = Shape::sphere;
    e.size = radius;
    e.falloff = falloff;
    e.edge_mm = 20.0f;
    return e;
}

}  // namespace

TEST_CASE("entity: shape coverage")
{
    Entity s = sphere(100.0f);
    CHECK(cover(s, {50.0f, 0.0f, 0.0f}) == 1.0f);
    CHECK(cover(s, {150.0f, 0.0f, 0.0f}) == 0.0f);

    Entity slab;
    slab.shape = Shape::slab;
    slab.size = 50.0f;
    slab.falloff = Falloff::hard;
    slab.pos = {0.0f, 0.0f, 1000.0f};
    CHECK(cover(slab, {900.0f, -400.0f, 1040.0f}) == 1.0f);   // anywhere horizontally
    CHECK(cover(slab, {0.0f, 0.0f, 1060.0f}) == 0.0f);

    Entity shell;
    shell.shape = Shape::shell;
    shell.size = 200.0f;
    shell.thickness = 20.0f;
    shell.falloff = Falloff::hard;
    CHECK(cover(shell, {210.0f, 0.0f, 0.0f}) == 1.0f);
    CHECK(cover(shell, {100.0f, 0.0f, 0.0f}) == 0.0f);   // hollow

    Entity cap;
    cap.shape = Shape::capsule;
    cap.size = 30.0f;
    cap.length = 200.0f;
    cap.axis = {1.0f, 0.0f, 0.0f};
    cap.falloff = Falloff::hard;
    CHECK(cover(cap, {190.0f, 20.0f, 0.0f}) == 1.0f);
    CHECK(cover(cap, {260.0f, 0.0f, 0.0f}) == 0.0f);

    Entity wedge;
    wedge.shape = Shape::wedge;
    wedge.size = 20.0f * pi / 180.0f;
    wedge.angle = 355.0f * pi / 180.0f;   // slice wraps through 0
    wedge.falloff = Falloff::hard;
    CHECK(cover(wedge, {300.0f * std::cos(0.1f), 300.0f * std::sin(0.1f), 500.0f}) == 1.0f);   // ~6 deg
    CHECK(cover(wedge, {0.0f, 300.0f, 500.0f}) == 0.0f);                                      // 90 deg
}

TEST_CASE("entity: falloffs")
{
    Entity e = sphere(100.0f, Falloff::linear);   // edge 20mm centred on the surface
    CHECK(cover(e, {90.0f, 0.0f, 0.0f}) == doctest::Approx(1.0f));
    CHECK(cover(e, {100.0f, 0.0f, 0.0f}) == doctest::Approx(0.5f));
    CHECK(cover(e, {110.0f, 0.0f, 0.0f}) == doctest::Approx(0.0f));
    e.falloff = Falloff::smooth;
    CHECK(cover(e, {100.0f, 0.0f, 0.0f}) == doctest::Approx(0.5f));
    CHECK(cover(e, {95.0f, 0.0f, 0.0f}) > 0.5f);
    e.falloff = Falloff::glow;
    CHECK(cover(e, {100.0f, 0.0f, 0.0f}) == 1.0f);
    CHECK(cover(e, {120.0f, 0.0f, 0.0f}) > 0.0f);   // a tail past the edge width
    CHECK(cover(e, {141.0f, 0.0f, 0.0f}) == 0.0f);
}

TEST_CASE("entity: gravity, drag and kinematic motion")
{
    setup(1000);
    Entity e = sphere(10.0f);
    e.pos = {0.0f, 0.0f, 1000.0f};
    Handle h = engine.spawn(e);
    engine.advance(100'000);   // 0.1 s
    Entity *p = engine.entity(h);
    REQUIRE(p != nullptr);
    CHECK(p->vel.z == doctest::Approx(-981.0f).epsilon(0.01));
    CHECK(p->pos.z == doctest::Approx(1000.0f - 0.5f * 9810.0f * 0.01f).epsilon(0.01));

    setup(1000);
    e.gravity_scale = 0.0f;
    e.vel = {1000.0f, 0.0f, 0.0f};
    e.drag = 1.0f;
    h = engine.spawn(e);
    engine.advance(1'000'000);
    CHECK(engine.entity(h)->vel.x == doctest::Approx(1000.0f * std::exp(-1.0f)).epsilon(0.01));

    setup(1000);
    Entity k = sphere(10.0f);
    k.kinematic = true;
    k.vel = {0.0f, 100.0f, 0.0f};
    h = engine.spawn(k);
    engine.advance(1'000'000);
    CHECK(engine.entity(h)->vel.z == 0.0f);   // no gravity
    CHECK(engine.entity(h)->pos.y == doctest::Approx(100.0f));
}

TEST_CASE("entity: floor responses")
{
    auto drop = [](Bound response) {
        setup(1000);
        Entity e = sphere(10.0f);
        e.pos = {0.0f, 0.0f, 100.0f};
        e.vel = {0.0f, 0.0f, -2000.0f};
        e.gravity_scale = 0.0f;
        e.restitution = 0.5f;
        e.floor = response;
        return engine.spawn(e);
    };

    Handle h = drop(Bound::bounce);
    engine.advance(100'000);
    CHECK(engine.entity(h)->vel.z == doctest::Approx(1000.0f));
    CHECK(engine.entity(h)->pos.z >= 0.0f);

    h = drop(Bound::stop);
    engine.advance(100'000);
    CHECK(engine.entity(h)->vel.z == 0.0f);
    CHECK(engine.entity(h)->pos.z == 0.0f);

    h = drop(Bound::destroy);
    engine.advance(100'000);
    CHECK(engine.entity(h) == nullptr);
    CHECK(engine.stats().entities == 0);

    h = drop(Bound::respawn);
    engine.advance(51'000);   // reaches the floor at 50 ms
    CHECK(engine.entity(h)->pos.z > 90.0f);
    CHECK(engine.entity(h)->vel.z == -2000.0f);

    h = drop(Bound::wrap);
    engine.advance(51'000);
    CHECK(engine.entity(h)->pos.z > 1900.0f);   // came back in at the top
    CHECK(engine.entity(h)->vel.z == -2000.0f);
}

TEST_CASE("entity: lifetime ends it; fades scale brightness")
{
    setup(100);
    Entity e = sphere(1000.0f, Falloff::hard);
    e.gravity_scale = 0.0f;
    e.lifetime_s = 1.0f;
    e.fade_in_s = 0.5f;
    e.pos = {0.0f, 0.0f, 1000.0f};
    int layer = engine.scene().add_layer(1, LayerType::entity);
    e.layer = static_cast<uint8_t>(layer);
    Handle h = engine.spawn(e);
    std::vector<Rgb> out(geometry.count());
    engine.advance(250'000);
    engine.render(out);
    CHECK(out[10].r == doctest::Approx(0.5f).epsilon(0.03));   // half faded in
    engine.advance(1'000'000);
    CHECK(engine.entity(h) == nullptr);
}

TEST_CASE("entity: surface constraint holds it on the envelope")
{
    setup(100);
    Entity e = sphere(10.0f);
    e.pos = {50.0f, 0.0f, 1500.0f};
    e.vel = {0.0f, 0.0f, -100.0f};
    e.gravity_scale = 0.0f;
    e.surface = Surface::envelope;
    Handle h = engine.spawn(e);
    engine.advance(500'000);
    const Entity *p = engine.entity(h);
    float r = std::sqrt(p->pos.x * p->pos.x + p->pos.y * p->pos.y);
    CHECK(r == doctest::Approx(geometry.envelope_radius(p->pos.z)).epsilon(0.001));
    // All LEDs sit at r = 300; the envelope comes from a 10mm histogram.
    CHECK(geometry.envelope_radius(1000.0f) >= 300.0f);
    CHECK(geometry.envelope_radius(1000.0f) <= 310.0f);
}

TEST_CASE("entity: outer bound bounces back inside the envelope")
{
    setup(1000);
    Entity e = sphere(10.0f);
    e.pos = {250.0f, 0.0f, 1000.0f};
    e.vel = {1000.0f, 0.0f, 0.0f};
    e.gravity_scale = 0.0f;
    e.restitution = 1.0f;
    e.outer = Bound::bounce;
    Handle h = engine.spawn(e);
    engine.advance(200'000);
    CHECK(engine.entity(h)->vel.x == doctest::Approx(-1000.0f));
    CHECK(engine.entity(h)->pos.x <= 300.0f);
}

TEST_CASE("entity: rendering culls by height without missing anything")
{
    setup(100);
    int layer = engine.scene().add_layer(1, LayerType::entity);
    for (int k = 0; k < 5; k++)
    {
        Entity e = sphere(180.0f + 30.0f * k, Falloff::smooth);
        e.edge_mm = 80.0f;
        e.layer = static_cast<uint8_t>(layer);
        e.gravity_scale = 0.0f;
        e.pos = {300.0f * std::cos(k * 1.3f), 300.0f * std::sin(k * 1.3f), 350.0f * k + 100.0f};
        engine.spawn(e);
    }
    std::vector<Rgb> out(geometry.count());
    engine.render(out);
    // Brute force: sum each entity's coverage at every LED (add combine, white).
    for (uint16_t i = 0; i < geometry.count(); i++)
    {
        float total = 0.0f;
        for (uint16_t k = 0; k < engine.entities().capacity; k++)
        {
            if (engine.entities().alive(k))
            {
                total += entity_coverage(engine.entities().item(k), geometry.position(i), geometry.radius(i),
                                         geometry.angle(i));
            }
        }
        CHECK(out[i].r == doctest::Approx(std::min(1.0f, total)).epsilon(0.001));
    }
}

TEST_CASE("entity: combine add vs max")
{
    setup(100);
    int layer = engine.scene().add_layer(1, LayerType::entity);
    Entity e = sphere(5000.0f);
    e.layer = static_cast<uint8_t>(layer);
    e.gravity_scale = 0.0f;
    e.color = {0.3f, 0.0f, 0.0f};
    engine.spawn(e);
    engine.spawn(e);
    std::vector<Rgb> out(geometry.count());
    engine.render(out);
    CHECK(out[0].r == doctest::Approx(0.6f));
    engine.scene().layer(1, static_cast<uint8_t>(layer))->combine = EntityCombine::max;
    engine.render(out);
    CHECK(out[0].r == doctest::Approx(0.3f));
}

TEST_CASE("entity: a full pool fails spawns and counts them")
{
    setup(100);
    Entity e = sphere(10.0f);
    for (uint16_t k = 0; k < max_entities; k++)
    {
        CHECK(engine.spawn(e).valid());
    }
    CHECK_FALSE(engine.spawn(e).valid());
    CHECK(engine.stats().spawns_failed == 1);
    engine.destroy_entities_in_slot(-1);
    CHECK(engine.entities().size() == 0);
}

TEST_CASE("modes: the launch sweep peaks at the top of the tree")
{
    setup(240);
    engine.director().set_slot(engine, 1, SlotSpec::of("sweep").set("motion", 2.0f), Transition::cut);
    REQUIRE(engine.stats().entities == 1);
    float peak = 0.0f, low = 1e9f;
    Handle h = engine.entities().handle_at(0);
    for (int step = 0; step < 240 * 3; step++)   // ~3 s: one full flight is ~2.8 s
    {
        engine.run_ticks(1);
        const Entity *e = engine.entity(h);
        REQUIRE(e != nullptr);
        peak = std::max(peak, e->pos.z);
        low = std::min(low, e->pos.z);
    }
    CHECK(peak == doctest::Approx(2000.0f).epsilon(0.01));
    CHECK(low >= 0.0f);   // respawned at the floor, never below it
}

TEST_CASE("modes: every mode sets up and runs, alone and with every parameter at its extremes")
{
    for (uint8_t m = 0; m < mode_count(); m++)
    {
        const ModeDef *def = mode_at(m);
        REQUIRE(def != nullptr);
        CHECK(find_mode(def->id) == m);
        for (int variant = 0; variant < 3; variant++)
        {
            setup(120);
            SlotSpec spec = SlotSpec::of(def->id);
            for (uint8_t p = 0; p < def->param_count; p++)
            {
                if (variant == 1)
                {
                    spec.params[p].f = def->params[p].min;
                }
                else if (variant == 2)
                {
                    spec.params[p].f = def->params[p].max;
                }
            }
            engine.director().set_slot(engine, 1, spec, Transition::cut);
            std::vector<Rgb> out(geometry.count());
            for (int f = 0; f < 120; f++)
            {
                engine.advance(16'667);
                engine.render(out);
            }
            CHECK(engine.stats().spawns_failed == 0);
        }
    }
}
