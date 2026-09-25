#include "neotree/modes.hpp"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "neotree/direct.hpp"
#include "neotree/director.hpp"
#include "neotree/engine.hpp"
#include "json.hpp"

namespace neotree {

namespace {

using detail::Json;

float deg(float d) { return d * pi / 180.0f; }

// ---- helpers shared by the modes ----

void backdrop(Scene &scene, uint8_t slot, Rgb color)
{
    int l = scene.add_layer(slot, LayerType::solid);
    if (l >= 0)
    {
        scene.layer(slot, static_cast<uint8_t>(l))->color = color;
    }
}

// Entity modes draw over whatever is below unless their backdrop is on.
void maybe_backdrop(ModeContext &ctx, uint8_t param, Rgb color)
{
    if (ctx.toggle(param))
    {
        backdrop(ctx.engine.scene(), ctx.slot, color);
    }
}

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

// Applies fn to every live entity in the slot.
template <typename Fn>
void for_slot_entities(Engine &engine, uint8_t slot, Fn fn)
{
    const EntityPool &pool = engine.entities();
    for (uint16_t k = 0; k < pool.capacity; k++)
    {
        if (pool.alive(k) && pool.item(k).slot == slot)
        {
            if (Entity *e = engine.entity(pool.handle_at(k)))
            {
                fn(*e);
            }
        }
    }
}

// The first layer of a type in the slot (or null).
Layer *layer_of(ModeContext &ctx, LayerType type)
{
    Slot *slot = ctx.engine.scene().slot(ctx.slot);
    for (uint8_t l = 0; slot != nullptr && l < slot->layer_count; l++)
    {
        if (slot->layers[l].type == type)
        {
            return &slot->layers[l];
        }
    }
    return nullptr;
}

// A rule counting a lifecycle cycle each time group g's entity crosses a boundary.
void cycle_on_boundary(ModeContext &ctx, uint8_t group, uint8_t which)
{
    Rule r;
    r.trigger = Trigger::boundary;
    r.group_a = group;
    r.id = which;
    Action a;
    a.type = ActionType::cycle;
    r.then(a);
    ctx.engine.behavior().add_rule(ctx.slot, r);
}

// ---- canvas: today's manual colors ----

void canvas_setup(ModeContext &ctx)
{
    Scene &scene = ctx.engine.scene();
    int background = scene.add_layer(ctx.slot, LayerType::pixel);
    int paint = scene.add_layer(ctx.slot, LayerType::pixel);
    if (background < 0 || paint < 0)
    {
        return;
    }
    auto bg = scene.pixels(ctx.slot, static_cast<uint8_t>(background));
    auto fg = scene.pixels(ctx.slot, static_cast<uint8_t>(paint));
    const Engine::CanvasMemory &memory = ctx.engine.canvas_memory();
    if (memory.saved)
    {
        // Back as it was when the Canvas last left the scene.
        for (size_t i = 0; i < bg.size() && i < fg.size(); i++)
        {
            bg[i] = memory.background[i];
            fg[i] = memory.paint[i];
        }
        return;
    }
    for (Rgba8 &px : bg)
    {
        px = {0, 0, 0, 255};
    }
    if (ctx.engine.config().canvas_seed != nullptr)
    {
        ctx.engine.config().canvas_seed(bg);
    }
}

// ---- solid / gradient / rainbow ----

const ParamDef solid_params[] = {
    {"color", "Color", ParamType::color, 0, 1, 0, 0, {1.0f, 0.55f, 0.2f}},
};
void solid_setup(ModeContext &ctx)
{
    backdrop(ctx.engine.scene(), ctx.slot, ctx.color(0));
}
bool solid_param(ModeContext &ctx, uint8_t)
{
    if (Layer *l = layer_of(ctx, LayerType::solid))
    {
        l->color = ctx.color(0);
        return true;
    }
    return false;
}

const ParamDef gradient_params[] = {
    {"bottom", "Bottom", ParamType::color, 0, 1, 0, 0, {0.0f, 0.1f, 0.6f}},
    {"top", "Top", ParamType::color, 0, 1, 0, 0, {0.6f, 0.0f, 0.4f}},
};
void gradient_setup(ModeContext &ctx)
{
    Scene &scene = ctx.engine.scene();
    int l = scene.add_layer(ctx.slot, LayerType::field);
    if (l >= 0)
    {
        Layer *f = scene.layer(ctx.slot, static_cast<uint8_t>(l));
        f->field.kind = FieldKind::height_gradient;
        f->field.color_a = ctx.color(0);
        f->field.color_b = ctx.color(1);
    }
}
bool gradient_param(ModeContext &ctx, uint8_t)
{
    if (Layer *f = layer_of(ctx, LayerType::field))
    {
        f->field.color_a = ctx.color(0);
        f->field.color_b = ctx.color(1);
        return true;
    }
    return false;
}

const ParamDef rainbow_params[] = {
    {"speed", "Spin (turns/s)", ParamType::number, -1.0f, 1.0f, 0.05f, 0.2f},
    {"bands", "Rainbows around", ParamType::number, 1.0f, 4.0f, 1.0f, 1.0f},
    {"saturation", "Saturation", ParamType::number, 0.0f, 1.0f, 0.05f, 1.0f},
    {"brightness", "Brightness", ParamType::number, 0.0f, 1.0f, 0.05f, 1.0f},
};
void rainbow_apply(ModeContext &ctx, Layer &f)
{
    f.field.kind = FieldKind::angle_rainbow;
    f.field.spin_rps = ctx.num(0);
    f.field.hue_cycles = ctx.num(1);
    f.field.saturation = ctx.num(2);
    f.field.value = ctx.num(3);
}
void rainbow_setup(ModeContext &ctx)
{
    Scene &scene = ctx.engine.scene();
    int l = scene.add_layer(ctx.slot, LayerType::field);
    if (l >= 0)
    {
        rainbow_apply(ctx, *scene.layer(ctx.slot, static_cast<uint8_t>(l)));
    }
}
bool rainbow_param(ModeContext &ctx, uint8_t)
{
    if (Layer *f = layer_of(ctx, LayerType::field))
    {
        rainbow_apply(ctx, *f);
        return true;
    }
    return false;
}

// ---- layers showcase (M2's demo) ----

void layers_setup(ModeContext &ctx)
{
    Scene &scene = ctx.engine.scene();
    const uint8_t slot = ctx.slot;
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
void layers_tick(ModeContext &ctx, float)
{
    Scene &scene = ctx.engine.scene();
    Layer *band = scene.layer(ctx.slot, 1);
    if (band == nullptr || band->type != LayerType::field)
    {
        return;
    }
    const World &w = ctx.engine.world();
    float t = static_cast<float>(static_cast<double>(ctx.engine.time_us()) * 1e-6);
    float h = 0.5f - 0.5f * std::cos(two_pi * t / 8.0f);   // up and down every 8 s
    float z = w.floor_z + h * (w.ceiling_z - w.floor_z);
    band->mask.z_min = z - 150.0f;
    band->mask.z_max = z + 150.0f;

    auto px = scene.pixels(ctx.slot, 3);
    if (px.empty())
    {
        return;
    }
    for (Rgba8 &p : px)
    {
        p.a = static_cast<uint8_t>(p.a * 0.93f);   // ~0.85 per frame at 60 fps
    }
    Rng &rng = ctx.engine.rng();
    for (int k = 0; k < 2; k++)
    {
        px[rng.below(static_cast<uint32_t>(ctx.engine.geometry().count()))] = {255, 255, 255, 255};
    }
}

// ---- lighthouse ----

const ParamDef lighthouse_params[] = {
    {"color", "Beam", ParamType::color, 0, 1, 0, 0, {1.0f, 0.8f, 0.4f}},
    {"speed", "Turns/s", ParamType::number, 0.05f, 2.0f, 0.05f, 0.5f},
    {"width", "Width (deg)", ParamType::number, 5.0f, 120.0f, 5.0f, 40.0f},
    {"backdrop", "Backdrop", ParamType::toggle, 0, 1, 1, 1},
};
void lighthouse_apply(ModeContext &ctx, Entity &beam)
{
    beam.color = ctx.color(0);
    beam.spin = two_pi * ctx.num(1);
    beam.size = deg(ctx.num(2) * 0.5f);
}
void lighthouse_setup(ModeContext &ctx)
{
    maybe_backdrop(ctx, 3, {0.0f, 0.0f, 0.05f});
    Entity beam;
    beam.slot = ctx.slot;
    beam.layer = entity_layer(ctx.engine.scene(), ctx.slot);
    beam.shape = Shape::wedge;
    beam.falloff = Falloff::smooth;
    beam.edge_mm = 120.0f;
    beam.kinematic = true;
    lighthouse_apply(ctx, beam);
    ctx.engine.spawn(beam);
}
bool lighthouse_param(ModeContext &ctx, uint8_t index)
{
    if (index == 3)
    {
        return false;   // backdrop: rebuild
    }
    for_slot_entities(ctx.engine, ctx.slot, [&](Entity &e) { lighthouse_apply(ctx, e); });
    return true;
}

// ---- sweep: the Pi display scripts as slab entities ----

// Their look: firmware green_____ backdrop, gravity 2000 mm/s^2
// (mapping/display_loop_sweep.py).
constexpr float sweep_gravity_mm_s2 = 2000.0f;
const ParamDef sweep_params[] = {
    {"motion", "Motion", ParamType::choice, 0, 2, 1, 0, {}, "linear|gravity|launch"},
    {"color", "Band", ParamType::color, 0, 1, 0, 0, {1.0f, 1.0f, 1.0f}},
    {"band", "Band (% of height)", ParamType::number, 5.0f, 40.0f, 2.5f, 12.5f},
    {"speed", "Passes/s (linear)", ParamType::number, 0.2f, 3.0f, 0.1f, 1.0f},
    {"backdrop", "Backdrop", ParamType::toggle, 0, 1, 1, 1},
};
void sweep_setup(ModeContext &ctx)
{
    maybe_backdrop(ctx, 4, to_rgb(0, 140, 0));
    const World &w = ctx.engine.world();
    const float height = w.ceiling_z - w.floor_z;
    Entity e;
    e.slot = ctx.slot;
    e.layer = entity_layer(ctx.engine.scene(), ctx.slot);
    e.shape = Shape::slab;
    e.size = height * ctx.num(2) / 200.0f;   // half the band
    e.falloff = Falloff::smooth;
    e.edge_mm = 60.0f;
    e.color = ctx.color(1);
    e.gravity_scale = 0.0f;
    e.group = 0;   // for the cycle rule (no collisions configured)
    switch (ctx.choice(0))
    {
    case 0:   // linear: top to bottom, wrapping
        e.pos = {0.0f, 0.0f, w.ceiling_z};
        e.vel = {0.0f, 0.0f, -height * ctx.num(3)};
        e.floor = Bound::wrap;
        break;
    case 1:   // gravity: from rest at the top, back to the top at the floor
        e.pos = {0.0f, 0.0f, w.ceiling_z};
        e.gravity_scale = sweep_gravity_mm_s2 / 9810.0f;
        e.floor = Bound::respawn;
        break;
    default:   // launch: v0 = sqrt(2 g h), apex exactly at the top
        e.pos = {0.0f, 0.0f, w.floor_z};
        e.vel = {0.0f, 0.0f, std::sqrt(2.0f * sweep_gravity_mm_s2 * height)};
        e.gravity_scale = sweep_gravity_mm_s2 / 9810.0f;
        e.floor = Bound::respawn;
        break;
    }
    ctx.engine.spawn(e);
    cycle_on_boundary(ctx, 0, hit_floor);   // one pass = one cycle
}

// ---- bounce ----

const ParamDef bounce_params[] = {
    {"count", "Balls", ParamType::number, 1, 12, 1, 6},
    {"gravity", "Gravity", ParamType::number, 0, 1, 0.05f, 0.35f},
    {"backdrop", "Backdrop", ParamType::toggle, 0, 1, 1, 1},
};
void bounce_setup(ModeContext &ctx)
{
    maybe_backdrop(ctx, 2, {0.0f, 0.0f, 0.0f});
    const uint8_t layer = entity_layer(ctx.engine.scene(), ctx.slot);
    const World &w = ctx.engine.world();
    Rng &rng = ctx.engine.rng();
    const int count = static_cast<int>(ctx.num(0));
    for (int k = 0; k < count; k++)
    {
        Entity b;
        b.slot = ctx.slot;
        b.layer = layer;
        b.size = 110.0f;
        b.edge_mm = 60.0f;
        b.color = hsv(360.0f * static_cast<float>(k) / static_cast<float>(count), 0.9f, 1.0f);
        float a = rng.range(0.0f, two_pi);
        float r = rng.range(0.0f, 200.0f);
        b.pos = {r * std::cos(a), r * std::sin(a), rng.range(w.floor_z + 400.0f, w.ceiling_z)};
        b.vel = {rng.range(-300.0f, 300.0f), rng.range(-300.0f, 300.0f), rng.range(-200.0f, 600.0f)};
        b.gravity_scale = ctx.num(1);
        b.restitution = 0.95f;
        b.floor = Bound::bounce;
        b.ceiling = Bound::bounce;
        b.outer = Bound::bounce;
        ctx.engine.spawn(b);
    }
}
bool bounce_param(ModeContext &ctx, uint8_t index)
{
    if (index != 1)
    {
        return false;
    }
    for_slot_entities(ctx.engine, ctx.slot, [&](Entity &e) { e.gravity_scale = ctx.num(1); });
    return true;
}

// ---- snow ----

const ParamDef snow_params[] = {
    {"rate", "Flakes/s", ParamType::number, 1, 40, 1, 12},
    {"wind", "Wind", ParamType::number, 0, 3, 0.1f, 1},
    {"backdrop", "Backdrop", ParamType::toggle, 0, 1, 1, 1},
};
void snow_setup(ModeContext &ctx)
{
    maybe_backdrop(ctx, 2, {0.0f, 0.01f, 0.04f});
    Behavior &b = ctx.engine.behavior();
    Entity flake;
    flake.layer = entity_layer(ctx.engine.scene(), ctx.slot, EntityCombine::max);
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
    int t = b.add_template(ctx.slot, flake);
    Emitter em;
    em.template_index = static_cast<uint8_t>(t);
    em.region = Region::top;
    em.rate = ctx.num(0);
    em.spread = 40.0f;
    em.size_jitter = 0.25f;
    em.lifetime_jitter = 0.15f;
    b.add_emitter(ctx.slot, em);
    b.set_quota(ctx.slot, 200);
}
void snow_tick(ModeContext &ctx, float)
{
    // Wind gusts: a slow wander of direction and strength.
    float t = static_cast<float>(static_cast<double>(ctx.engine.time_us()) * 1e-6);
    float strength = ctx.num(1) * (60.0f + 50.0f * std::sin(t * 0.7f));
    float dir = 0.4f * std::sin(t * 0.13f);
    ctx.engine.forces().wind = {strength * std::cos(dir), strength * std::sin(dir), 0.0f};
}
bool snow_param(ModeContext &ctx, uint8_t index)
{
    if (index == 0)
    {
        if (Emitter *em = ctx.engine.behavior().emitter(ctx.slot, 0))
        {
            em->rate = ctx.num(0);
        }
        return true;
    }
    return index == 1;   // wind: read every tick
}

// ---- orbit ----

const ParamDef orbit_params[] = {
    {"count", "Comets", ParamType::number, 1, 8, 1, 4},
    {"speed", "Speed", ParamType::number, 0.2f, 4.0f, 0.1f, 1.2f},
    {"backdrop", "Backdrop", ParamType::toggle, 0, 1, 1, 1},
};
void orbit_setup(ModeContext &ctx)
{
    maybe_backdrop(ctx, 2, {0.01f, 0.0f, 0.02f});
    const uint8_t layer = entity_layer(ctx.engine.scene(), ctx.slot);
    const World &w = ctx.engine.world();
    const Rgb colors[] = {{1.0f, 0.5f, 0.1f}, {0.2f, 0.6f, 1.0f}, {1.0f, 0.2f, 0.6f}, {0.4f, 1.0f, 0.4f}};
    const int count = static_cast<int>(ctx.num(0));
    for (int k = 0; k < count; k++)
    {
        Entity c;
        c.slot = ctx.slot;
        c.layer = layer;
        c.shape = Shape::capsule;
        c.size = 110.0f;
        c.length = 200.0f;
        c.align_to_velocity = true;
        c.edge_mm = 80.0f;
        c.color = colors[k % 4];
        float z = w.floor_z + (w.ceiling_z - w.floor_z) * (0.15f + 0.7f * static_cast<float>(k) / static_cast<float>(count));
        float a = static_cast<float>(k) * two_pi / static_cast<float>(count);
        float r = ctx.engine.geometry().envelope_radius(z) - 60.0f;
        c.pos = {r * std::cos(a), r * std::sin(a), z};
        c.vel = {0.0f, 0.0f, (k % 2 ? 1.0f : -1.0f) * 150.0f};
        c.gravity_scale = 0.0f;
        c.surface = Surface::envelope;
        c.surface_offset_mm = -60.0f;
        c.floor = Bound::bounce;
        c.ceiling = Bound::bounce;
        c.restitution = 1.0f;
        ctx.engine.spawn(c);
    }
}
void orbit_tick(ModeContext &ctx, float)
{
    ctx.engine.forces().swirl_rad_s = ctx.num(1);
    ctx.engine.forces().swirl_rate = 3.0f;
}

// ---- fireworks ----

const ParamDef fireworks_params[] = {
    {"rate", "Rockets/s", ParamType::number, 0.2f, 3.0f, 0.1f, 0.9f},
    {"sparks", "Sparks", ParamType::number, 8, 60, 1, 32},
    {"backdrop", "Backdrop", ParamType::toggle, 0, 1, 1, 1},
};
void fireworks_setup(ModeContext &ctx)
{
    maybe_backdrop(ctx, 2, {0.0f, 0.0f, 0.02f});
    Behavior &b = ctx.engine.behavior();
    const World &w = ctx.engine.world();
    const uint8_t layer = entity_layer(ctx.engine.scene(), ctx.slot);

    Entity rocket;
    rocket.layer = layer;
    rocket.size = 45.0f;
    rocket.edge_mm = 50.0f;
    rocket.gravity_scale = 0.35f;
    rocket.lifetime_s = 0.85f;   // bursts near the top of its climb, ~1.3 m up
    rocket.group = 0;
    int rocket_t = b.add_template(ctx.slot, rocket);

    Entity spark;
    spark.layer = layer;
    spark.size = 70.0f;          // ~LED spacing, so sparks reliably light LEDs
    spark.edge_mm = 80.0f;
    spark.gravity_scale = 0.18f;
    spark.drag = 1.6f;
    spark.lifetime_s = 1.6f;
    spark.fade_out_s = 1.2f;
    spark.group = 1;
    int spark_t = b.add_template(ctx.slot, spark);

    Emitter launcher;
    launcher.template_index = static_cast<uint8_t>(rocket_t);
    launcher.region = Region::band;
    launcher.min = {0.0f, 0.0f, w.floor_z};
    launcher.max = {180.0f, 0.0f, w.floor_z};
    launcher.rate = ctx.num(0);
    launcher.vel = {0.0f, 0.0f, 3000.0f};
    launcher.spread = 350.0f;
    launcher.lifetime_jitter = 0.15f;
    launcher.color_from = ColorFrom::random_each;
    b.add_emitter(ctx.slot, launcher);

    // When a rocket's life ends it bursts: sparks in its color, plus white
    // glitter. Each burst is a lifecycle cycle.
    Rule burst;
    burst.trigger = Trigger::expired;
    burst.group_a = 0;
    Action sparks;
    sparks.type = ActionType::spawn;
    sparks.index = static_cast<uint8_t>(spark_t);
    sparks.count = static_cast<uint8_t>(ctx.num(1));
    sparks.place = Place::a;
    sparks.color_from = ColorFrom::a;
    sparks.spread = 1000.0f;
    sparks.inherit = 0.3f;
    Action glitter = sparks;
    glitter.count = static_cast<uint8_t>(ctx.num(1) / 4.0f);
    glitter.color_from = ColorFrom::fixed;
    glitter.color = {1.0f, 1.0f, 0.9f};
    glitter.spread = 700.0f;
    Action cycle;
    cycle.type = ActionType::cycle;
    burst.then(sparks).then(glitter).then(cycle);
    b.add_rule(ctx.slot, burst);
    b.set_quota(ctx.slot, 200);
}
bool fireworks_param(ModeContext &ctx, uint8_t index)
{
    if (index == 0)
    {
        if (Emitter *em = ctx.engine.behavior().emitter(ctx.slot, 0))
        {
            em->rate = ctx.num(0);
        }
        return true;
    }
    if (index == 1)
    {
        if (Rule *r = ctx.engine.behavior().rule(ctx.slot, 0))
        {
            r->actions[0].count = static_cast<uint8_t>(ctx.num(1));
            r->actions[1].count = static_cast<uint8_t>(ctx.num(1) / 4.0f);
        }
        return true;
    }
    return false;
}

// ---- chain ----

const ParamDef chain_params[] = {
    {"max", "Most balls", ParamType::number, 5, 80, 1, 40},
    {"backdrop", "Backdrop", ParamType::toggle, 0, 1, 1, 1},
};
void chain_setup(ModeContext &ctx)
{
    maybe_backdrop(ctx, 1, {0.0f, 0.0f, 0.0f});
    Behavior &b = ctx.engine.behavior();
    const World &w = ctx.engine.world();
    Entity ball;
    ball.layer = entity_layer(ctx.engine.scene(), ctx.slot);
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
    int t = b.add_template(ctx.slot, ball);
    b.set_response(ctx.slot, 0, 0, Response::bounce);

    // Every collision can spawn another ball right there - a chain reaction.
    // The slot quota is what keeps it bounded.
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
    b.add_rule(ctx.slot, grow);
    b.set_quota(ctx.slot, static_cast<uint16_t>(ctx.num(0)));

    // Five starters in a ring at mid-height, all heading for the trunk, so
    // the first collisions come within a second or two.
    Rng &rng = ctx.engine.rng();
    const float mid = 0.5f * (w.floor_z + w.ceiling_z);
    for (int k = 0; k < 5; k++)
    {
        Entity n = ball;
        float a = two_pi * static_cast<float>(k) / 5.0f;
        n.pos = {250.0f * std::cos(a), 250.0f * std::sin(a), mid + rng.range(-250.0f, 250.0f)};
        n.vel = {-500.0f * std::cos(a), -500.0f * std::sin(a), rng.range(-200.0f, 200.0f)};
        n.color = hsv(72.0f * static_cast<float>(k), 1.0f, 1.0f);
        b.spawn(ctx.engine, ctx.slot, n);
    }
}
bool chain_param(ModeContext &ctx, uint8_t index)
{
    if (index != 0)
    {
        return false;
    }
    ctx.engine.behavior().set_quota(ctx.slot, static_cast<uint16_t>(ctx.num(0)));
    return true;
}

// ---- mixer ----

const ParamDef mixer_params[] = {
    {"count", "Balls", ParamType::number, 2, 16, 2, 8},
    {"backdrop", "Backdrop", ParamType::toggle, 0, 1, 1, 1},
};
void mixer_setup(ModeContext &ctx)
{
    maybe_backdrop(ctx, 1, {0.01f, 0.01f, 0.01f});
    Behavior &b = ctx.engine.behavior();
    const World &w = ctx.engine.world();
    Entity ball;
    ball.layer = entity_layer(ctx.engine.scene(), ctx.slot);
    ball.size = 110.0f;
    ball.edge_mm = 60.0f;
    ball.gravity_scale = 0.0f;
    ball.restitution = 1.0f;
    ball.floor = Bound::bounce;
    ball.ceiling = Bound::bounce;
    ball.outer = Bound::bounce;
    b.set_response(ctx.slot, 0, 0, Response::bounce);
    b.set_response(ctx.slot, 1, 1, Response::bounce);
    b.set_response(ctx.slot, 0, 1, Response::overlap);

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
    b.add_rule(ctx.slot, swap);

    Rng &rng = ctx.engine.rng();
    const int count = static_cast<int>(ctx.num(0));
    for (int k = 0; k < count; k++)
    {
        Entity n = ball;
        n.group = static_cast<uint8_t>(k % 2);
        n.color = n.group == 0 ? Rgb{1.0f, 0.1f, 0.05f} : Rgb{0.1f, 0.2f, 1.0f};
        float z = rng.range(w.floor_z + 200.0f, w.ceiling_z - 200.0f);
        n.pos = {rng.range(-150.0f, 150.0f), rng.range(-150.0f, 150.0f), z};
        n.vel = {rng.range(-450.0f, 450.0f), rng.range(-450.0f, 450.0f), rng.range(-450.0f, 450.0f)};
        b.spawn(ctx.engine, ctx.slot, n);
    }
}

// ---- the registry ----

// ---- play: what phones throw and paint (M7, neotree/direct.hpp) ----

const ParamDef play_params[] = {
    {"fade", "Trail fade (s, 0 = keep)", ParamType::number, 0, 30, 0.5f, 4},
    {"gravity", "Ball gravity", ParamType::number, 0, 2, 0.1f, 1},
    {"bounce", "Ball bounce", ParamType::number, 0, 1, 0.05f, 0.7f},
    {"look", "Ball look", ParamType::choice, 0, 2, 1, 0, {}, "solid|glow|bubble"},
    {"hit", "When balls meet", ParamType::choice, 0, 3, 1, 0, {}, "bounce|pass|burst|mix"},
    {"tails", "Comet tails", ParamType::toggle, 0, 1, 1, 0},
    {"air", "Air drag", ParamType::number, 0, 3, 0.05f, 0.15f},
    {"life", "Ball life (s)", ParamType::number, 3, 60, 1, 20},
};
enum PlayParam : uint8_t
{
    play_fade,
    play_gravity,
    play_bounce,
    play_look,
    play_hit,
    play_tails,
    play_air,
    play_life,
};
enum PlayLook : int
{
    look_solid,
    look_glow,
    look_bubble,
};
enum PlayHit : int
{
    hit_bounce,
    hit_pass,
    hit_burst,
    hit_mix,
};
constexpr uint8_t play_trail_layer = 0;
constexpr uint8_t play_spark_template = 2;
// Real gravity drops a ball the height of the tree in about half a second:
// balls fall at a quarter of it (times the gravity parameter).
constexpr float play_gravity_scale = 0.25f;
// Per slot: trail alpha still to take off, carried between ticks (a tick's
// share of a fade is usually a fraction of one step of alpha); the rules for
// "burst" and "mix" (both set up, one or none enabled).
float play_decay[max_slots] = {};
int play_burst_rule[max_slots] = {-1, -1, -1, -1};
int play_mix_rule[max_slots] = {-1, -1, -1, -1};

// A phone's ball - not the mode's own sparks, or a brush.
bool is_ball(const Entity &e)
{
    return e.owner != 0 && e.direct_kind == static_cast<uint8_t>(DirectKind::ball);
}

void play_stroke(ModeContext &ctx, Vec3 from, Vec3 to, Rgb color, float radius);

// A ball's look, motion and life from the parameters (size is the phone's).
void style_ball(const ModeContext &ctx, Entity &e)
{
    e.gravity_scale = play_gravity_scale * ctx.num(play_gravity);
    e.restitution = ctx.num(play_bounce);
    e.drag = ctx.num(play_air);
    e.lifetime_s = ctx.num(play_life);
    switch (ctx.choice(play_look))
    {
    case look_glow:
        e.shape = Shape::sphere;
        e.falloff = Falloff::glow;
        e.edge_mm = 55.0f;
        break;
    case look_bubble:
        e.shape = Shape::shell;
        e.falloff = Falloff::smooth;
        e.thickness = 16.0f;
        e.edge_mm = 30.0f;
        break;
    default:
        e.shape = Shape::sphere;
        e.falloff = Falloff::smooth;
        e.edge_mm = 60.0f;
        break;
    }
}

// Applies the parameters to the ball template, every ball in flight, and the
// collision setup - live, so balls and trails carry on.
void play_apply(ModeContext &ctx)
{
    Behavior &b = ctx.engine.behavior();
    if (Entity *ball = b.template_at(ctx.slot, static_cast<uint8_t>(DirectKind::ball)))
    {
        style_ball(ctx, *ball);
    }
    for_slot_entities(ctx.engine, ctx.slot, [&](Entity &e) {
        if (is_ball(e))
        {
            style_ball(ctx, e);
        }
    });
    const int hit = ctx.choice(play_hit);
    b.set_response(ctx.slot, 0, 0, hit == hit_pass ? Response::ignore : Response::bounce);
    if (Rule *r = play_burst_rule[ctx.slot] >= 0 ? b.rule(ctx.slot, static_cast<uint8_t>(play_burst_rule[ctx.slot])) : nullptr)
    {
        r->enabled = hit == hit_burst;
    }
    if (Rule *r = play_mix_rule[ctx.slot] >= 0 ? b.rule(ctx.slot, static_cast<uint8_t>(play_mix_rule[ctx.slot])) : nullptr)
    {
        r->enabled = hit == hit_mix;
    }
}

void play_setup(ModeContext &ctx)
{
    Scene &scene = ctx.engine.scene();
    const int trail = scene.add_layer(ctx.slot, LayerType::pixel);
    if (trail >= 0)
    {
        for (Rgba8 &px : scene.pixels(ctx.slot, static_cast<uint8_t>(trail)))
        {
            px = {0, 0, 0, 0};
        }
    }
    const uint8_t layer = entity_layer(scene, ctx.slot, EntityCombine::max);
    Behavior &b = ctx.engine.behavior();

    Entity ball;   // template 0; styled by play_apply
    ball.layer = layer;
    ball.size = 70.0f;
    ball.floor = Bound::bounce;
    ball.ceiling = Bound::bounce;
    ball.outer = Bound::bounce;
    ball.fade_in_s = 0.1f;
    ball.fade_out_s = 3.0f;
    ball.group = 0;
    b.add_template(ctx.slot, ball);

    Entity brush;   // template 1: moved by the phone, never by forces
    brush.layer = layer;
    brush.size = 80.0f;
    brush.edge_mm = 50.0f;
    brush.kinematic = true;
    brush.gravity_scale = 0.0f;
    brush.lifetime_s = brush_lease_s;   // each sample starts it over
    brush.fade_out_s = 0.3f;
    brush.group = 1;
    b.add_template(ctx.slot, brush);

    Entity spark;   // template 2: what a burst leaves; no collisions
    spark.layer = layer;
    spark.size = 45.0f;
    spark.edge_mm = 55.0f;
    spark.gravity_scale = 0.2f;
    spark.drag = 1.2f;
    spark.lifetime_s = 1.4f;
    spark.fade_out_s = 1.0f;
    b.add_template(ctx.slot, spark);

    // Balls bounce off brushes (which, being kinematic, bat them like an
    // immovable paddle); between balls it's the "hit" parameter.
    b.set_response(ctx.slot, 0, 1, Response::bounce);

    // Burst: both balls go, in sparks of their colors.
    Rule burst;
    burst.trigger = Trigger::collision;
    burst.group_a = 0;
    burst.group_b = 0;
    Action from_a;
    from_a.type = ActionType::spawn;
    from_a.index = play_spark_template;
    from_a.count = 8;
    from_a.place = Place::event_point;
    from_a.color_from = ColorFrom::a;
    from_a.spread = 1300.0f;
    from_a.inherit = 0.3f;
    Action from_b = from_a;
    from_b.color_from = ColorFrom::b;
    Action gone;
    gone.type = ActionType::destroy;
    gone.target = Target::both;
    burst.then(from_a).then(from_b).then(gone);
    play_burst_rule[ctx.slot] = b.add_rule(ctx.slot, burst);

    // Mix: both take the color halfway between them, and bounce apart.
    Rule mix;
    mix.trigger = Trigger::collision;
    mix.group_a = 0;
    mix.group_b = 0;
    Action blend;
    blend.type = ActionType::set_color;
    blend.target = Target::both;
    blend.color_from = ColorFrom::mix;
    mix.then(blend);
    play_mix_rule[ctx.slot] = b.add_rule(ctx.slot, mix);

    b.set_quota(ctx.slot, 160);
    play_decay[ctx.slot] = 0.0f;
    play_apply(ctx);
}

void play_tick(ModeContext &ctx, float dt)
{
    // Comet tails: each ball paints its path since the last tick.
    if (ctx.toggle(play_tails))
    {
        for_slot_entities(ctx.engine, ctx.slot, [&](Entity &e) {
            if (is_ball(e))
            {
                play_stroke(ctx, e.spawn_pos, e.pos, e.color, std::max(0.8f * e.size, 70.0f));
            }
        });
    }
    // (spawn_pos: a ball never respawns, so it holds where the tail got to.)
    for_slot_entities(ctx.engine, ctx.slot, [&](Entity &e) {
        if (is_ball(e))
        {
            e.spawn_pos = e.pos;
        }
    });

    const float fade = ctx.num(play_fade);
    if (fade <= 0.0f)
    {
        return;   // strokes are kept
    }
    // Linear fade: full alpha to none over `fade` seconds.
    float &acc = play_decay[ctx.slot];
    acc += 255.0f * dt / fade;
    const int steps = static_cast<int>(acc);
    if (steps == 0)
    {
        return;
    }
    acc -= static_cast<float>(steps);
    std::span<Rgba8> px = ctx.engine.scene().pixels(ctx.slot, play_trail_layer);
    for (uint16_t i : ctx.engine.geometry().z_order())
    {
        const int a = px[i].a;
        px[i].a = static_cast<uint8_t>(a > steps ? a - steps : 0);
    }
}

bool play_param(ModeContext &ctx, uint8_t index)
{
    if (index != play_fade)
    {
        play_apply(ctx);
    }
    return true;   // all live; fade and tails are read every tick
}

// Paints a soft round dab into px at p (a point on the tree's surface): full
// color inside half the radius, fading to nothing at the radius. Distance is
// measured on the surface - each LED pushed out along its own radius to the
// envelope - so a dab lights every LED under it, however deep inside the
// tree, as someone looking at the tree sees it.
void stamp(const LedGeometry &geometry, std::span<Rgba8> px, Vec3 p, Rgb color, float radius)
{
    // The centre goes onto the surface too: brush samples are already there,
    // but a ball's tail is painted from wherever inside the tree it flies.
    const float rp = std::sqrt(p.x * p.x + p.y * p.y);
    if (rp > 1.0f)
    {
        const float out = geometry.envelope_radius(p.z) / rp;
        p.x *= out;
        p.y *= out;
    }
    const float r2 = radius * radius;
    const float inv_edge = 2.0f / radius;
    for (uint16_t i : geometry.in_z_range(p.z - radius, p.z + radius))
    {
        if (i >= px.size())
        {
            continue;
        }
        const float z = geometry.z(i);
        const float out = geometry.envelope_radius(z) / std::max(geometry.radius(i), 1.0f);
        const float dx = geometry.x(i) * out - p.x;
        const float dy = geometry.y(i) * out - p.y;
        const float dz = z - p.z;
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > r2)
        {
            continue;
        }
        const float cover = clamp01((radius - std::sqrt(d2)) * inv_edge);
        const uint8_t a = unit_to_byte(cover);
        if (a == 0)
        {
            continue;
        }
        Rgba8 &q = px[i];
        const Rgb old = q.a != 0 ? to_rgb(q.r, q.g, q.b) : color;
        const Rgb mixed = lerp(old, color, cover);
        q = {unit_to_byte(mixed.r), unit_to_byte(mixed.g), unit_to_byte(mixed.b), q.a > a ? q.a : a};
    }
}

void play_stroke(ModeContext &ctx, Vec3 from, Vec3 to, Rgb color, float radius)
{
    Engine &engine = ctx.engine;
    std::span<Rgba8> target = engine.scene().pixels(ctx.slot, play_trail_layer);
    if (ctx.num(0) <= 0.0f)
    {
        // Kept strokes are paint on the Colors canvas (its paint layer) -
        // the Home page's paint, there after this mode is gone.
        for (uint8_t s = 0; s < max_slots; s++)
        {
            const Director::SlotInfo &info = engine.director().slot(s);
            if (info.state != SlotState::empty && info.spec.mode == find_mode("canvas"))
            {
                target = engine.scene().pixels(s, 1);
                break;
            }
        }
    }
    if (target.empty() || radius <= 0.0f)
    {
        return;
    }
    // Dabs along the segment, close enough together to join up.
    const Vec3 d{to.x - from.x, to.y - from.y, to.z - from.z};
    const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    const int steps = std::min(64, 1 + static_cast<int>(len / (0.35f * radius)));
    for (int k = 0; k <= steps; k++)
    {
        const float t = static_cast<float>(k) / static_cast<float>(steps);
        stamp(engine.geometry(), target, Vec3{from.x + d.x * t, from.y + d.y * t, from.z + d.z * t}, color, radius);
    }
}

#define PARAMS(p) p, static_cast<uint8_t>(sizeof(p) / sizeof(p[0]))

const ModeDef modes[] = {
    {"canvas", "Colors", "Your background and paint colors from the Home page", nullptr, 0, canvas_setup},
    {"solid", "Solid", "One color", PARAMS(solid_params), solid_setup, nullptr, solid_param},
    {"gradient", "Gradient", "Blends from one color at the bottom to another at the top", PARAMS(gradient_params),
     gradient_setup, nullptr, gradient_param},
    {"rainbow", "Rainbow", "A rainbow around the trunk, spinning", PARAMS(rainbow_params), rainbow_setup, nullptr,
     rainbow_param},
    {"layers", "Layers showcase", "Night sky, a rainbow band riding up and down, warm tips, sparkles", nullptr, 0,
     layers_setup, layers_tick},
    {"lighthouse", "Lighthouse", "A beam sweeping around the tree", PARAMS(lighthouse_params), lighthouse_setup,
     nullptr, lighthouse_param},
    {"sweep", "Sweep", "A band of light moving through the tree", PARAMS(sweep_params), sweep_setup},
    {"bounce", "Bounce", "Balls bouncing inside the tree", PARAMS(bounce_params), bounce_setup, nullptr,
     bounce_param},
    {"snow", "Snow", "Flakes drifting down in the wind, settling and fading", PARAMS(snow_params), snow_setup,
     snow_tick, snow_param},
    {"orbit", "Orbit", "Comets circling the outside of the tree", PARAMS(orbit_params), orbit_setup, orbit_tick},
    {"fireworks", "Fireworks", "Rockets that burst into sparks", PARAMS(fireworks_params), fireworks_setup, nullptr,
     fireworks_param},
    {"chain", "Chain reaction", "Every collision makes another ball", PARAMS(chain_params), chain_setup, nullptr,
     chain_param},
    {"mixer", "Color mixer", "Red and blue balls swap colors as they pass through each other",
     PARAMS(mixer_params), mixer_setup},
    {"play", "Play", "Flick balls and paint from the phone", PARAMS(play_params),
     play_setup, play_tick, play_param, true, play_stroke},
};
constexpr uint8_t count_of_modes = static_cast<uint8_t>(sizeof(modes) / sizeof(modes[0]));

}  // namespace

uint8_t mode_count() { return count_of_modes; }

const ModeDef *mode_at(uint8_t index) { return index < count_of_modes ? &modes[index] : nullptr; }

uint8_t find_mode(const char *id)
{
    for (uint8_t i = 0; i < count_of_modes; i++)
    {
        if (std::strcmp(modes[i].id, id) == 0)
        {
            return i;
        }
    }
    return no_mode;
}

void default_params(const ModeDef &def, ParamValue out[max_params])
{
    for (uint8_t i = 0; i < max_params; i++)
    {
        out[i] = {};
        if (i < def.param_count)
        {
            out[i].f = def.params[i].def;
            out[i].c = def.params[i].def_color;
        }
    }
}

size_t describe_modes(char *out, size_t cap)
{
    if (cap == 0)
    {
        return 0;
    }
    Json j{out, cap};
    j.raw("{\"modes\":[");
    for (uint8_t i = 0; i < count_of_modes; i++)
    {
        const ModeDef &m = modes[i];
        j.raw("%s{\"id\":\"%s\",\"n\":\"%s\",\"s\":\"%s\",\"p\":[", i ? "," : "", m.id, m.name, m.summary);
        for (uint8_t p = 0; p < m.param_count; p++)
        {
            const ParamDef &d = m.params[p];
            j.raw("%s{\"id\":\"%s\",\"l\":\"%s\",", p ? "," : "", d.id, d.label);
            switch (d.type)
            {
            case ParamType::number:
                j.raw("\"t\":\"n\"");
                if (d.min != 0.0f)
                {
                    j.raw(",\"min\":");
                    j.num(d.min);
                }
                if (d.max != 1.0f)
                {
                    j.raw(",\"max\":");
                    j.num(d.max);
                }
                if (d.step != 0.0f)
                {
                    j.raw(",\"st\":");
                    j.num(d.step);
                }
                j.raw(",\"d\":");
                j.num(d.def);
                j.raw("}");
                break;
            case ParamType::color:
                j.raw("\"t\":\"c\",\"d\":");
                j.hex(d.def_color);
                j.raw("}");
                break;
            case ParamType::choice:
                j.raw("\"t\":\"ch\",\"ch\":\"%s\",\"d\":", d.choices ? d.choices : "");
                j.num(d.def);
                j.raw("}");
                break;
            case ParamType::toggle:
                j.raw(d.def >= 0.5f ? "\"t\":\"t\",\"d\":true}" : "\"t\":\"t\"}");
                break;
            }
        }
        j.raw("]}");
    }
    j.raw("],\"presets\":[");
    for (uint8_t i = 0; i < preset_count(); i++)
    {
        j.raw("%s\"%s\"", i ? "," : "", preset_at(i).name);
    }
    j.raw("]}");
    return j.len;
}

}  // namespace neotree
