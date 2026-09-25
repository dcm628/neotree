#include "neotree/demos.hpp"

#include <cmath>
#include <cstring>

namespace neotree {

namespace {

const char *const names[] = {"none",   "layers", "wedge", "sweep_linear", "sweep_gravity", "sweep_launch",
                             "bounce", "snow",   "orbit", "fireworks",    "chain",         "mixer"};
static_assert(sizeof(names) / sizeof(names[0]) == static_cast<size_t>(Demo::count));

float deg(float d) { return d * pi / 180.0f; }

// An opaque solid backdrop as the slot's bottom layer, hiding what's below.
void backdrop(Scene &scene, uint8_t slot, Rgb color)
{
    int l = scene.add_layer(slot, LayerType::solid);
    if (l >= 0)
    {
        scene.layer(slot, static_cast<uint8_t>(l))->color = color;
    }
}

// Adds an entity layer; returns its index (or 0 if the slot is full).
uint8_t entity_layer(Scene &scene, uint8_t slot, EntityCombine combine = EntityCombine::add)
{
    int l = scene.add_layer(slot, LayerType::entity);
    if (l < 0)
    {
        return 0;
    }
    scene.layer(slot, static_cast<uint8_t>(l))->combine = combine;
    return static_cast<uint8_t>(l);
}

// The Pi display scripts' look: firmware green_____ backdrop, band 1/8 of
// the tree's height, gravity 2000 mm/s^2 (display_loop_sweep.py).
const Rgb sweep_backdrop = to_rgb(0, 140, 0);
const Rgb sweep_color = to_rgb(255, 255, 255);
constexpr float sweep_gravity_mm_s2 = 2000.0f;

Entity sweep_slab(Engine &engine, uint8_t slot, uint8_t layer)
{
    const World &w = engine.world();
    Entity e;
    e.slot = slot;
    e.layer = layer;
    e.shape = Shape::slab;
    e.axis = {0.0f, 0.0f, 1.0f};
    e.size = (w.ceiling_z - w.floor_z) / 16.0f;   // half of 1/8
    e.falloff = Falloff::smooth;
    e.edge_mm = 60.0f;
    e.color = sweep_color;
    e.gravity_scale = 0.0f;
    return e;
}

void setup_layers(Engine &engine, uint8_t slot)
{
    Scene &scene = engine.scene();
    int l = scene.add_layer(slot, LayerType::field);
    Layer *sky = scene.layer(slot, static_cast<uint8_t>(l));
    sky->field.kind = FieldKind::height_gradient;
    sky->field.color_a = {0.02f, 0.02f, 0.12f};
    sky->field.color_b = {0.10f, 0.00f, 0.18f};

    l = scene.add_layer(slot, LayerType::field);
    Layer *band = scene.layer(slot, static_cast<uint8_t>(l));
    band->field.kind = FieldKind::angle_rainbow;
    band->field.spin_rps = 0.3f;
    band->field.hue_cycles = 2.0f;
    band->mask = Mask::make_cylinder(0.0f, 300.0f, 0.0f, 10000.0f, 0.0f, two_pi, 150.0f);

    l = scene.add_layer(slot, LayerType::solid);
    Layer *tips = scene.layer(slot, static_cast<uint8_t>(l));
    tips->color = {0.6f, 0.45f, 0.2f};
    tips->blend = Blend::max;
    tips->opacity = 0.5f;
    tips->mask = Mask::make_cylinder(-10000.0f, 10000.0f, 400.0f, 10000.0f, 0.0f, two_pi, 60.0f);

    l = scene.add_layer(slot, LayerType::pixel);
    if (l >= 0)
    {
        scene.layer(slot, static_cast<uint8_t>(l))->blend = Blend::add;
    }
}

void update_layers(Engine &engine, uint8_t slot)
{
    Scene &scene = engine.scene();
    Layer *band = scene.layer(slot, 1);
    if (band == nullptr || band->type != LayerType::field)
    {
        return;
    }
    const World &w = engine.world();
    float t = static_cast<float>(static_cast<double>(engine.time_us()) * 1e-6);
    float h = 0.5f - 0.5f * std::cos(two_pi * t / 8.0f);   // up and down every 8 s
    float z = w.floor_z + h * (w.ceiling_z - w.floor_z);
    band->mask.z_min = z - 150.0f;
    band->mask.z_max = z + 150.0f;

    auto px = scene.pixels(slot, 3);
    if (px.empty())
    {
        return;
    }
    for (Rgba8 &p : px)
    {
        p.a = static_cast<uint8_t>(p.a * 0.85f);
    }
    Rng &rng = engine.rng();
    for (int k = 0; k < 4; k++)
    {
        px[rng.below(static_cast<uint32_t>(engine.geometry().count()))] = {255, 255, 255, 255};
    }
}

void setup_wedge(Engine &engine, uint8_t slot)
{
    backdrop(engine.scene(), slot, {0.0f, 0.0f, 0.05f});
    Entity beam;
    beam.slot = slot;
    beam.layer = entity_layer(engine.scene(), slot);
    beam.shape = Shape::wedge;
    beam.size = deg(20.0f);
    beam.falloff = Falloff::smooth;
    beam.edge_mm = 120.0f;
    beam.color = {1.0f, 0.8f, 0.4f};
    beam.spin = pi;   // half a turn per second
    beam.kinematic = true;
    engine.spawn(beam);
}

void setup_sweep(Engine &engine, uint8_t slot, Demo kind)
{
    backdrop(engine.scene(), slot, sweep_backdrop);
    const World &w = engine.world();
    Entity e = sweep_slab(engine, slot, entity_layer(engine.scene(), slot));
    float height = w.ceiling_z - w.floor_z;
    switch (kind)
    {
    case Demo::sweep_linear:
        // One top-to-bottom pass per second, wrapping back to the top.
        e.pos = {0.0f, 0.0f, w.ceiling_z};
        e.vel = {0.0f, 0.0f, -height};
        e.floor = Bound::wrap;
        break;
    case Demo::sweep_gravity:
        // From rest at the top; back to the top on reaching the floor.
        e.pos = {0.0f, 0.0f, w.ceiling_z};
        e.gravity_scale = sweep_gravity_mm_s2 / 9810.0f;
        e.floor = Bound::respawn;
        break;
    default:   // sweep_launch
        // v0 = sqrt(2 g h): apex exactly at the top, then back down.
        e.pos = {0.0f, 0.0f, w.floor_z};
        e.vel = {0.0f, 0.0f, std::sqrt(2.0f * sweep_gravity_mm_s2 * height)};
        e.gravity_scale = sweep_gravity_mm_s2 / 9810.0f;
        e.floor = Bound::respawn;
        break;
    }
    engine.spawn(e);
}

void setup_bounce(Engine &engine, uint8_t slot)
{
    backdrop(engine.scene(), slot, {0.0f, 0.0f, 0.0f});
    uint8_t layer = entity_layer(engine.scene(), slot);
    const World &w = engine.world();
    const Rgb colors[] = {{1.0f, 0.1f, 0.1f}, {0.1f, 1.0f, 0.2f}, {0.2f, 0.3f, 1.0f},
                          {1.0f, 0.8f, 0.1f}, {0.9f, 0.2f, 1.0f}, {0.1f, 0.9f, 0.9f}};
    Rng &rng = engine.rng();
    for (int k = 0; k < 6; k++)
    {
        Entity b;
        b.slot = slot;
        b.layer = layer;
        b.shape = Shape::sphere;
        b.size = 110.0f;
        b.edge_mm = 60.0f;
        b.color = colors[k];
        float a = rng.range(0.0f, two_pi);
        float r = rng.range(0.0f, 200.0f);
        b.pos = {r * std::cos(a), r * std::sin(a), rng.range(w.floor_z + 400.0f, w.ceiling_z)};
        b.vel = {rng.range(-300.0f, 300.0f), rng.range(-300.0f, 300.0f), rng.range(-200.0f, 600.0f)};
        b.gravity_scale = 0.35f;
        b.restitution = 0.95f;
        b.floor = Bound::bounce;
        b.ceiling = Bound::bounce;
        b.outer = Bound::bounce;
        engine.spawn(b);
    }
}

void setup_snow(Engine &engine, uint8_t slot)
{
    backdrop(engine.scene(), slot, {0.0f, 0.01f, 0.04f});
    Behavior &b = engine.behavior();
    Entity flake;
    flake.layer = entity_layer(engine.scene(), slot, EntityCombine::max);
    flake.shape = Shape::sphere;
    // LEDs are ~80mm apart on average, so flakes have to be about that big to
    // reliably light one.
    flake.size = 65.0f;
    flake.edge_mm = 60.0f;
    flake.color = {0.8f, 0.85f, 1.0f};
    flake.vel = {0.0f, 0.0f, -150.0f};
    flake.gravity_scale = 0.008f;   // with the drag: falls at ~260 mm/s
    flake.drag = 0.3f;
    flake.floor = Bound::stop;      // settles...
    flake.outer = Bound::stop;
    flake.lifetime_s = 12.0f;       // ...and fades away where it lies
    flake.fade_in_s = 0.5f;
    flake.fade_out_s = 3.0f;
    int t = b.add_template(slot, flake);
    Emitter em;
    em.template_index = static_cast<uint8_t>(t);
    em.region = Region::top;
    em.rate = 12.0f;
    em.spread = 40.0f;
    em.size_jitter = 0.25f;
    em.lifetime_jitter = 0.15f;
    b.add_emitter(slot, em);
    b.set_quota(slot, 180);
}

void update_snow(Engine &engine)
{
    // Wind gusts: a slow wander of direction and strength.
    float t = static_cast<float>(static_cast<double>(engine.time_us()) * 1e-6);
    float strength = 60.0f + 50.0f * std::sin(t * 0.7f);
    float dir = 0.4f * std::sin(t * 0.13f);
    engine.forces().wind = {strength * std::cos(dir), strength * std::sin(dir), 0.0f};
}

void setup_orbit(Engine &engine, uint8_t slot)
{
    backdrop(engine.scene(), slot, {0.01f, 0.0f, 0.02f});
    uint8_t layer = entity_layer(engine.scene(), slot);
    const World &w = engine.world();
    engine.forces().swirl_rad_s = 1.2f;
    engine.forces().swirl_rate = 3.0f;
    const Rgb colors[] = {{1.0f, 0.5f, 0.1f}, {0.2f, 0.6f, 1.0f}, {1.0f, 0.2f, 0.6f}, {0.4f, 1.0f, 0.4f}};
    for (int k = 0; k < 4; k++)
    {
        Entity c;
        c.slot = slot;
        c.layer = layer;
        c.shape = Shape::capsule;
        c.size = 110.0f;
        c.length = 200.0f;
        c.align_to_velocity = true;
        c.edge_mm = 80.0f;
        c.color = colors[k];
        float z = w.floor_z + (w.ceiling_z - w.floor_z) * (0.2f + 0.2f * k);
        float a = k * pi / 2.0f;
        float r = engine.geometry().envelope_radius(z) - 60.0f;
        c.pos = {r * std::cos(a), r * std::sin(a), z};
        c.vel = {0.0f, 0.0f, (k % 2 ? 1.0f : -1.0f) * 150.0f};
        c.gravity_scale = 0.0f;
        c.surface = Surface::envelope;
        c.surface_offset_mm = -60.0f;
        c.floor = Bound::bounce;
        c.ceiling = Bound::bounce;
        c.restitution = 1.0f;
        engine.spawn(c);
    }
}

void setup_fireworks(Engine &engine, uint8_t slot)
{
    backdrop(engine.scene(), slot, {0.0f, 0.0f, 0.02f});
    Behavior &b = engine.behavior();
    const World &w = engine.world();
    const uint8_t layer = entity_layer(engine.scene(), slot);

    Entity rocket;
    rocket.layer = layer;
    rocket.size = 45.0f;
    rocket.edge_mm = 50.0f;
    rocket.gravity_scale = 0.35f;
    rocket.lifetime_s = 0.85f;  // bursts near the top of its climb, ~1.3 m up
    rocket.group = 0;
    int rocket_t = b.add_template(slot, rocket);

    Entity spark;
    spark.layer = layer;
    spark.size = 70.0f;         // ~LED spacing, so sparks reliably light LEDs
    spark.edge_mm = 80.0f;
    spark.gravity_scale = 0.18f;
    spark.drag = 1.6f;
    spark.lifetime_s = 1.6f;
    spark.fade_out_s = 1.2f;
    spark.group = 1;
    int spark_t = b.add_template(slot, spark);

    Emitter launcher;
    launcher.template_index = static_cast<uint8_t>(rocket_t);
    launcher.region = Region::band;
    launcher.min = {0.0f, 0.0f, w.floor_z};
    launcher.max = {180.0f, 0.0f, w.floor_z};
    launcher.rate = 0.9f;
    launcher.vel = {0.0f, 0.0f, 3000.0f};
    launcher.spread = 350.0f;
    launcher.lifetime_jitter = 0.15f;
    launcher.color_from = ColorFrom::random_each;
    b.add_emitter(slot, launcher);

    // When a rocket's life ends it bursts: sparks in its color, plus white glitter.
    Rule burst;
    burst.trigger = Trigger::expired;
    burst.group_a = 0;
    Action sparks;
    sparks.type = ActionType::spawn;
    sparks.index = static_cast<uint8_t>(spark_t);
    sparks.count = 32;
    sparks.place = Place::a;
    sparks.color_from = ColorFrom::a;
    sparks.spread = 1000.0f;
    sparks.inherit = 0.3f;
    Action glitter = sparks;
    glitter.count = 8;
    glitter.color_from = ColorFrom::fixed;
    glitter.color = {1.0f, 1.0f, 0.9f};
    glitter.spread = 700.0f;
    burst.then(sparks).then(glitter);
    b.add_rule(slot, burst);
    b.set_quota(slot, 200);
}

void setup_chain(Engine &engine, uint8_t slot)
{
    backdrop(engine.scene(), slot, {0.0f, 0.0f, 0.0f});
    Behavior &b = engine.behavior();
    const World &w = engine.world();
    Entity ball;
    ball.layer = entity_layer(engine.scene(), slot);
    ball.size = 100.0f;
    ball.edge_mm = 60.0f;
    ball.gravity_scale = 0.0f;   // floating, perfectly bouncy: they keep filling the tree
    ball.restitution = 1.0f;
    ball.floor = Bound::bounce;
    ball.ceiling = Bound::bounce;
    ball.outer = Bound::bounce;
    ball.lifetime_s = 25.0f;
    ball.fade_in_s = 0.3f;
    ball.fade_out_s = 2.0f;
    ball.group = 0;
    int t = b.add_template(slot, ball);
    b.set_response(slot, 0, 0, Response::bounce);

    // Every collision can spawn another ball right there - a chain reaction.
    // The slot quota (40) is what keeps it bounded.
    Rule grow;
    grow.trigger = Trigger::collision;
    grow.group_a = 0;
    grow.group_b = 0;
    grow.probability = 0.6f;
    grow.cooldown_s = 0.15f;
    Action more;
    more.type = ActionType::spawn;
    more.index = static_cast<uint8_t>(t);
    more.spread = 700.0f;
    more.color_from = ColorFrom::random_each;
    grow.then(more);
    b.add_rule(slot, grow);
    b.set_quota(slot, 40);

    // Five starters in a ring at mid-height, all heading for the trunk, so
    // the first collisions come within a second or two.
    Rng &rng = engine.rng();
    const float mid = 0.5f * (w.floor_z + w.ceiling_z);
    for (int k = 0; k < 5; k++)
    {
        Entity n = ball;
        float a = two_pi * static_cast<float>(k) / 5.0f;
        n.pos = {250.0f * std::cos(a), 250.0f * std::sin(a), mid + rng.range(-250.0f, 250.0f)};
        n.vel = {-500.0f * std::cos(a), -500.0f * std::sin(a), rng.range(-200.0f, 200.0f)};
        n.color = hsv(72.0f * static_cast<float>(k), 1.0f, 1.0f);
        b.spawn(engine, slot, n);
    }
}

void setup_mixer(Engine &engine, uint8_t slot)
{
    backdrop(engine.scene(), slot, {0.01f, 0.01f, 0.01f});
    Behavior &b = engine.behavior();
    const World &w = engine.world();
    Entity ball;
    ball.layer = entity_layer(engine.scene(), slot);
    ball.size = 110.0f;
    ball.edge_mm = 60.0f;
    ball.gravity_scale = 0.0f;
    ball.restitution = 1.0f;
    ball.floor = Bound::bounce;
    ball.ceiling = Bound::bounce;
    ball.outer = Bound::bounce;
    b.set_response(slot, 0, 0, Response::bounce);
    b.set_response(slot, 1, 1, Response::bounce);
    b.set_response(slot, 0, 1, Response::overlap);

    // Passing through: swap colors. The event holds both colors from before
    // the swap, so the two actions don't see each other's result.
    Rule swap;
    swap.trigger = Trigger::overlap_begin;
    swap.group_a = 0;
    swap.group_b = 1;
    Action to_a;
    to_a.type = ActionType::set_color;
    to_a.target = Target::a;
    to_a.color_from = ColorFrom::b;
    Action to_b = to_a;
    to_b.target = Target::b;
    to_b.color_from = ColorFrom::a;
    swap.then(to_a).then(to_b);
    b.add_rule(slot, swap);

    Rng &rng = engine.rng();
    for (int k = 0; k < 8; k++)
    {
        Entity n = ball;
        n.group = static_cast<uint8_t>(k % 2);
        n.color = n.group == 0 ? Rgb{1.0f, 0.1f, 0.05f} : Rgb{0.1f, 0.2f, 1.0f};
        float z = rng.range(w.floor_z + 200.0f, w.ceiling_z - 200.0f);
        n.pos = {rng.range(-150.0f, 150.0f), rng.range(-150.0f, 150.0f), z};
        n.vel = {rng.range(-450.0f, 450.0f), rng.range(-450.0f, 450.0f), rng.range(-450.0f, 450.0f)};
        b.spawn(engine, slot, n);
    }
}

}  // namespace

const char *demo_name(Demo demo)
{
    auto i = static_cast<size_t>(demo);
    return i < static_cast<size_t>(Demo::count) ? names[i] : "?";
}

bool demo_from_name(const char *name, Demo &out)
{
    for (size_t i = 0; i < static_cast<size_t>(Demo::count); i++)
    {
        if (std::strcmp(name, names[i]) == 0)
        {
            out = static_cast<Demo>(i);
            return true;
        }
    }
    return false;
}

const char *demo_names()
{
    return "none, layers, wedge, sweep_linear, sweep_gravity, sweep_launch, bounce, snow, orbit, fireworks, "
           "chain, mixer";
}

void setup_demo(Engine &engine, Demo demo, uint8_t slot)
{
    engine.scene().clear_slot(slot);
    engine.destroy_entities_in_slot(slot);
    engine.behavior().clear_slot(slot);
    engine.forces() = Forces{};
    switch (demo)
    {
    case Demo::none: break;
    case Demo::layers: setup_layers(engine, slot); break;
    case Demo::wedge: setup_wedge(engine, slot); break;
    case Demo::sweep_linear:
    case Demo::sweep_gravity:
    case Demo::sweep_launch: setup_sweep(engine, slot, demo); break;
    case Demo::bounce: setup_bounce(engine, slot); break;
    case Demo::snow: setup_snow(engine, slot); break;
    case Demo::orbit: setup_orbit(engine, slot); break;
    case Demo::fireworks: setup_fireworks(engine, slot); break;
    case Demo::chain: setup_chain(engine, slot); break;
    case Demo::mixer: setup_mixer(engine, slot); break;
    case Demo::count: break;
    }
}

void update_demo(Engine &engine, Demo demo, uint8_t slot)
{
    switch (demo)
    {
    case Demo::layers: update_layers(engine, slot); break;
    case Demo::snow: update_snow(engine); break;
    default: break;
    }
}

}  // namespace neotree
