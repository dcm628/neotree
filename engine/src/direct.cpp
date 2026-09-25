#include "neotree/direct.hpp"

#include <cmath>

#include "neotree/engine.hpp"

namespace neotree {

namespace {

// A brush's velocity (for collisions: it can bat balls) is measured from its
// samples, capped so a jump in aim isn't a hit at warp speed.
constexpr float max_brush_speed = 6000.0f;   // mm/s
// Between samples (30-60 a second) a brush keeps moving at its measured
// velocity, so it glides rather than steps; past this without one, it stops.
constexpr float coast_s = 0.05f;

// The running mode of the slot, if it offers direct control.
const ModeDef *direct_mode(const Engine &engine, uint8_t slot)
{
    if (slot >= max_slots)
    {
        return nullptr;
    }
    const Director::SlotInfo &info = engine.director().slot(slot);
    const ModeDef *def = mode_at(info.spec.mode);
    const bool live = info.state == SlotState::running || info.state == SlotState::entering;
    return live && def != nullptr && def->direct ? def : nullptr;
}

Entity *find(Engine &engine, uint8_t owner, uint8_t id, DirectKind kind)
{
    EntityPool &pool = engine.entities_mut();
    for (uint16_t k = 0; k < pool.capacity; k++)
    {
        if (pool.alive(k))
        {
            Entity &e = pool.item(k);
            if (e.owner == owner && e.direct_id == id && e.direct_kind == static_cast<uint8_t>(kind))
            {
                return &e;
            }
        }
    }
    return nullptr;
}

}  // namespace

bool direct_spawn(Engine &engine, uint8_t slot, uint8_t owner, uint8_t id, DirectKind kind, Vec3 pos, Vec3 vel,
                  Rgb color, float size_mm)
{
    if (owner == 0 || direct_mode(engine, slot) == nullptr)
    {
        return false;
    }
    const Entity *tmpl = engine.behavior().template_at(slot, static_cast<uint8_t>(kind));
    if (tmpl == nullptr)
    {
        return false;
    }
    EntityPool &pool = engine.entities_mut();
    // The same id again replaces it; past the per-phone limit the oldest goes.
    uint16_t balls = 0;
    int oldest = -1;
    for (uint16_t k = 0; k < pool.capacity; k++)
    {
        if (!pool.alive(k) || pool.item(k).owner != owner || pool.item(k).direct_kind != static_cast<uint8_t>(kind))
        {
            continue;
        }
        if (pool.item(k).direct_id == id)
        {
            pool.destroy(pool.handle_at(k));
            continue;
        }
        balls++;
        if (oldest < 0 || pool.item(k).age_s > pool.item(static_cast<uint16_t>(oldest)).age_s)
        {
            oldest = k;
        }
    }
    if (kind == DirectKind::ball && balls >= max_direct_balls && oldest >= 0)
    {
        pool.destroy(pool.handle_at(static_cast<uint16_t>(oldest)));
    }
    Entity e = *tmpl;
    e.pos = pos;
    e.vel = vel;
    e.color = color;
    if (size_mm > 0.0f)
    {
        e.size = size_mm;
    }
    e.owner = owner;
    e.direct_id = id;
    e.direct_kind = static_cast<uint8_t>(kind);
    return engine.behavior().spawn(engine, slot, e).valid();
}

bool direct_brush(Engine &engine, uint8_t slot, uint8_t owner, uint8_t id, const BrushSample &sample)
{
    const ModeDef *def = direct_mode(engine, slot);
    if (owner == 0 || def == nullptr)
    {
        return false;
    }
    Entity *e = find(engine, owner, id, DirectKind::brush);
    if (e == nullptr)
    {
        if (!direct_spawn(engine, slot, owner, id, DirectKind::brush, sample.pos, Vec3{}, sample.color,
                          sample.radius_mm))
        {
            return false;
        }
        e = find(engine, owner, id, DirectKind::brush);
        if (e == nullptr)
        {
            return false;
        }
    }
    // From the last sample, not from where the brush has glided to since:
    // measuring from an overshoot would turn the velocity around.
    // (spawn_pos: a brush never respawns, so it holds its last sample.)
    const Vec3 from = e->spawn_pos;
    const Vec3 to = sample.pos;
    if (sample.pen_down && def->stroke != nullptr)
    {
        // A stroke continues from the last sample if the pen was already
        // down; a fresh touch starts with a dot.
        ModeContext ctx{engine, slot, *def, engine.director().slot(slot).spec.params};
        def->stroke(ctx, e->pen_down ? from : to, to, sample.color, sample.radius_mm);
    }
    // Velocity from the move since the last sample (age_s counts from it).
    const float dt = e->age_s > 1e-3f ? e->age_s : 1e-3f;
    Vec3 v{(to.x - from.x) / dt, (to.y - from.y) / dt, (to.z - from.z) / dt};
    const float speed = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (speed > max_brush_speed)
    {
        const float k = max_brush_speed / speed;
        v = {v.x * k, v.y * k, v.z * k};
    }
    e->vel = v;
    e->pos = to;
    e->spawn_pos = to;
    e->color = sample.color;
    e->size = sample.radius_mm;
    // Dim while hovering (the phone's aim), full while painting.
    e->brightness = sample.pen_down ? 1.0f : 0.35f;
    e->pen_down = sample.pen_down;
    e->age_s = 0.0f;   // the lease starts over
    return true;
}

uint16_t direct_kill(Engine &engine, uint8_t owner, uint8_t id)
{
    if (owner == 0)
    {
        return 0;
    }
    EntityPool &pool = engine.entities_mut();
    uint16_t n = 0;
    for (uint16_t k = 0; k < pool.capacity; k++)
    {
        if (pool.alive(k) && pool.item(k).owner == owner && (id == direct_all || pool.item(k).direct_id == id))
        {
            pool.destroy(pool.handle_at(k));
            n++;
        }
    }
    return n;
}

void direct_tick(Engine &engine)
{
    EntityPool &pool = engine.entities_mut();
    for (uint16_t k = 0; k < pool.capacity; k++)
    {
        if (!pool.alive(k))
        {
            continue;
        }
        Entity &e = pool.item(k);
        if (e.owner != 0 && e.direct_kind == static_cast<uint8_t>(DirectKind::brush) && e.age_s > coast_s)
        {
            e.vel = Vec3{};
        }
    }
}

uint16_t direct_count(const Engine &engine)
{
    const EntityPool &pool = engine.entities();
    uint16_t n = 0;
    for (uint16_t k = 0; k < pool.capacity; k++)
    {
        n += pool.alive(k) && pool.item(k).owner != 0;
    }
    return n;
}

}  // namespace neotree
