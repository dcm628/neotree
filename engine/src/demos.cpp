#include "neotree/demos.hpp"

#include <cmath>
#include <cstring>

namespace neotree {

namespace {

const char *const names[] = {"none", "layers", "wedge", "sweep_linear", "sweep_gravity",
                             "sweep_launch", "bounce", "snow", "orbit"};
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
    uint8_t layer = entity_layer(engine.scene(), slot, EntityCombine::max);
    const World &w = engine.world();
    Rng &rng = engine.rng();
    for (int k = 0; k < 150; k++)
    {
        Entity f;
        f.slot = slot;
        f.layer = layer;
        f.shape = Shape::sphere;
        // LEDs are ~80mm apart on average, so flakes have to be about that
        // big to reliably light one.
        f.size = rng.range(50.0f, 80.0f);
        f.edge_mm = 60.0f;
        f.color = {0.8f, 0.85f, 1.0f};
        f.brightness = rng.range(0.4f, 1.0f);
        float a = rng.range(0.0f, two_pi);
        float z = rng.range(w.floor_z, w.ceiling_z);
        float r = rng.range(0.0f, 1.05f) * engine.geometry().envelope_radius(z);
        f.pos = {r * std::cos(a), r * std::sin(a), z};
        f.vel = {0.0f, 0.0f, rng.range(-250.0f, -120.0f)};
        f.gravity_scale = 0.008f;   // with the drag: falls at ~260 mm/s
        f.drag = 0.3f;
        f.wind_scale = rng.range(0.5f, 1.5f);
        f.floor = Bound::wrap;
        f.outer = Bound::stop;
        engine.spawn(f);
    }
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
    return "none, layers, wedge, sweep_linear, sweep_gravity, sweep_launch, bounce, snow, orbit";
}

void setup_demo(Engine &engine, Demo demo, uint8_t slot)
{
    engine.scene().clear_slot(slot);
    engine.destroy_entities_in_slot(slot);
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
