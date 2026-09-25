#include "neotree/entity.hpp"

#include <algorithm>
#include <cmath>

#include "neotree/color.hpp"
#include "neotree/hot.hpp"

namespace neotree {

namespace {

Vec3 add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 scale(Vec3 a, float k) { return {a.x * k, a.y * k, a.z * k}; }
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float length(Vec3 a) { return std::sqrt(dot(a, a)); }

Vec3 normalized_or(Vec3 v, Vec3 fallback)
{
    float len = length(v);
    return len > 1e-6f ? scale(v, 1.0f / len) : fallback;
}

// How far past the surface (d > 0) an entity still lights anything.
float falloff_reach(const Entity &e)
{
    switch (e.falloff)
    {
    case Falloff::hard: return 0.0f;
    case Falloff::linear:
    case Falloff::smooth: return 0.5f * e.edge_mm;
    case Falloff::glow: return 2.0f * e.edge_mm;
    }
    return 0.0f;
}

NEOTREE_INLINE float coverage_from_distance(const Entity &e, float d)
{
    const float w = std::max(e.edge_mm, 1e-3f);
    switch (e.falloff)
    {
    case Falloff::hard:
        return d <= 0.0f ? 1.0f : 0.0f;
    case Falloff::linear:
        return std::clamp(0.5f - d / w, 0.0f, 1.0f);
    case Falloff::smooth:
    {
        float s = std::clamp(0.5f - d / w, 0.0f, 1.0f);
        return s * s * (3.0f - 2.0f * s);
    }
    case Falloff::glow:
    {
        if (d <= 0.0f)
        {
            return 1.0f;
        }
        float k = d / w;
        return d >= 2.0f * w ? 0.0f : std::exp(-2.0f * k * k);
    }
    }
    return 0.0f;
}

// The shape's axis this frame (capsules and slabs can follow their velocity).
Vec3 effective_axis(const Entity &e)
{
    Vec3 axis = normalized_or(e.axis, Vec3{0.0f, 0.0f, 1.0f});
    return e.align_to_velocity ? normalized_or(e.vel, axis) : axis;
}

// Signed distance from p to the shape's surface (negative inside), mm.
float shape_distance(const Entity &e, Vec3 axis, Vec3 p, float led_radius, float led_angle)
{
    switch (e.shape)
    {
    case Shape::sphere:
        return length(sub(p, e.pos)) - e.size;
    case Shape::slab:
        return std::fabs(dot(sub(p, e.pos), axis)) - e.size;
    case Shape::shell:
        return std::fabs(length(sub(p, e.pos)) - e.size) - e.thickness;
    case Shape::capsule:
    {
        Vec3 rel = sub(p, e.pos);
        float t = std::clamp(dot(rel, axis), -e.length, e.length);
        return length(sub(rel, scale(axis, t))) - e.size;
    }
    case Shape::wedge:
    {
        // Angular distance past the slice's edge, as arc length at the LED.
        float d = std::fabs(led_angle - e.angle);
        d = std::fmod(d, two_pi);
        d = std::min(d, two_pi - d);
        return (d - e.size) * led_radius;
    }
    }
    return 1e9f;
}

// Half-height of the shape plus its falloff reach, for culling; negative
// means the whole tree (it isn't bounded in z).
float z_reach(const Entity &e, Vec3 axis)
{
    const float reach = falloff_reach(e);
    switch (e.shape)
    {
    case Shape::sphere: return e.size + reach;
    case Shape::shell: return e.size + e.thickness + reach;
    case Shape::capsule: return std::fabs(axis.z) * e.length + e.size + reach;
    case Shape::slab:
        // Only a (near-)horizontal slab is bounded in z.
        return std::fabs(axis.z) > 0.999f ? (e.size + reach) / std::fabs(axis.z) : -1.0f;
    case Shape::wedge: return -1.0f;
    }
    return -1.0f;
}

float life_fade(const Entity &e)
{
    float k = 1.0f;
    if (e.fade_in_s > 0.0f)
    {
        k = std::min(k, e.age_s / e.fade_in_s);
    }
    if (e.fade_out_s > 0.0f && e.lifetime_s > 0.0f)
    {
        k = std::min(k, (e.lifetime_s - e.age_s) / e.fade_out_s);
    }
    return std::clamp(k, 0.0f, 1.0f);
}

// exp(-x) for the per-tick drag factor. x = drag * dt is tiny (0.3 / 120),
// where a short series is exact to float precision and skips the library
// call (~430 ns on the Pico - per entity, per tick).
float decay(float x)
{
    if (x < 0.1f)
    {
        return 1.0f - x * (1.0f - x * (0.5f - x * (1.0f / 6.0f - x * (1.0f / 24.0f))));
    }
    return std::exp(-x);
}

void respawn(Entity &e)
{
    e.pos = e.spawn_pos;
    e.vel = e.spawn_vel;
    e.age_s = 0.0f;
}

// Applies a floor/ceiling response. inward = +1 for the floor (inside is
// above), -1 for the ceiling. Returns false to destroy.
bool vertical_bound(Entity &e, Bound response, float limit, float inward, const World &world)
{
    switch (response)
    {
    case Bound::pass:
        break;
    case Bound::stop:
        e.pos.z = limit;
        if (e.vel.z * inward < 0.0f)
        {
            e.vel.z = 0.0f;
        }
        break;
    case Bound::bounce:
        e.pos.z = limit + (limit - e.pos.z);
        if (e.vel.z * inward < 0.0f)
        {
            e.vel.z = -e.vel.z * e.restitution;
        }
        break;
    case Bound::destroy:
        return false;
    case Bound::respawn:
        respawn(e);
        break;
    case Bound::wrap:
        e.pos.z += inward * (world.ceiling_z - world.floor_z);
        break;
    }
    return true;
}

}  // namespace

float entity_coverage(const Entity &e, Vec3 p, float led_radius, float led_angle)
{
    return coverage_from_distance(e, shape_distance(e, effective_axis(e), p, led_radius, led_angle));
}

float collision_radius(const Entity &e)
{
    if (e.collide_radius > 0.0f)
    {
        return e.collide_radius;
    }
    switch (e.shape)
    {
    case Shape::sphere: return e.size;
    case Shape::capsule: return e.size + e.length;
    default: return 0.0f;   // slabs, shells and wedges only collide if given a radius
    }
}

StepResult step_entity(Entity &e, const Forces &forces, const World &world, const LedGeometry &geometry, float dt)
{
    StepResult result;
    e.age_s += dt;
    if (e.lifetime_s > 0.0f && e.age_s >= e.lifetime_s)
    {
        result.alive = false;
        result.expired = true;
        return result;
    }
    if (e.spin != 0.0f)
    {
        // Back into [0, 2pi): fmod(a, 2pi) with a = angle + turn + 2pi. A
        // tick turns a fraction of a turn, so a is almost always in
        // [0, 4pi), where fmod is one exact subtraction (Sterbenz) - the
        // same bits, without fmodf's ~570 ns on the Pico.
        float a = e.angle + e.spin * dt + two_pi;
        if (a >= two_pi && a < 2.0f * two_pi)
        {
            a -= two_pi;
        }
        else if (a < 0.0f || a >= two_pi)
        {
            a = std::fmod(a, two_pi);
        }
        e.angle = a;
    }

    if (!e.kinematic)
    {
        Vec3 a = add(scale(forces.gravity, e.gravity_scale), scale(forces.wind, e.wind_scale));
        if (forces.swirl_rate != 0.0f && e.swirl_scale != 0.0f)
        {
            float r = std::sqrt(e.pos.x * e.pos.x + e.pos.y * e.pos.y);
            if (r > 1.0f)
            {
                Vec3 radial{e.pos.x / r, e.pos.y / r, 0.0f};
                Vec3 tangent{-radial.y, radial.x, 0.0f};
                float vt = dot(e.vel, tangent);
                float want = forces.swirl_rad_s * r;
                a = add(a, scale(tangent, (want - vt) * forces.swirl_rate * e.swirl_scale));
                a = add(a, scale(radial, -vt * vt / r * e.swirl_scale));
            }
        }
        e.vel = add(e.vel, scale(a, dt));
        if (e.drag > 0.0f)
        {
            e.vel = scale(e.vel, decay(e.drag * dt));
        }
    }
    e.pos = add(e.pos, scale(e.vel, dt));

    // Surface constraint: onto the envelope, radial motion removed.
    if (e.surface == Surface::envelope)
    {
        float r = std::sqrt(e.pos.x * e.pos.x + e.pos.y * e.pos.y);
        Vec3 radial = r > 1e-3f ? Vec3{e.pos.x / r, e.pos.y / r, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
        float target = geometry.envelope_radius(e.pos.z) + e.surface_offset_mm;
        e.pos.x = radial.x * target;
        e.pos.y = radial.y * target;
        float vr = dot(e.vel, radial);
        e.vel = sub(e.vel, scale(radial, vr));
    }

    if (e.pos.z < world.floor_z)
    {
        result.hits |= hit_floor;
        if (!vertical_bound(e, e.floor, world.floor_z, 1.0f, world))
        {
            result.alive = false;
            return result;
        }
    }
    if (e.pos.z > world.ceiling_z)
    {
        result.hits |= hit_ceiling;
        if (!vertical_bound(e, e.ceiling, world.ceiling_z, -1.0f, world))
        {
            result.alive = false;
            return result;
        }
    }

    if (e.outer != Bound::pass)
    {
        float r = std::sqrt(e.pos.x * e.pos.x + e.pos.y * e.pos.y);
        float limit = geometry.envelope_radius(e.pos.z) + world.outer_margin_mm;
        if (r > limit && r > 1e-3f)
        {
            result.hits |= hit_outer;
            Vec3 radial{e.pos.x / r, e.pos.y / r, 0.0f};
            float vr = dot(e.vel, radial);
            switch (e.outer)
            {
            case Bound::destroy:
                result.alive = false;
                return result;
            case Bound::respawn:
                respawn(e);
                break;
            case Bound::bounce:
            {
                float back = limit - (r - limit);
                e.pos.x = radial.x * back;
                e.pos.y = radial.y * back;
                if (vr > 0.0f)
                {
                    e.vel = sub(e.vel, scale(radial, (1.0f + e.restitution) * vr));
                }
                break;
            }
            default:   // stop, wrap
                e.pos.x = radial.x * limit;
                e.pos.y = radial.y * limit;
                if (vr > 0.0f)
                {
                    e.vel = sub(e.vel, scale(radial, vr));
                }
                break;
            }
        }
    }
    return result;
}

// ---- rendering ----

namespace {

template <EntityCombine Combine>
NEOTREE_INLINE void accumulate(EntityScratch &s, uint16_t i, Rgb c, float cov)
{
    if constexpr (Combine == EntityCombine::add)
    {
        s.color[i] = {s.color[i].r + c.r * cov, s.color[i].g + c.g * cov, s.color[i].b + c.b * cov};
        s.alpha[i] = min_f(1.0f, s.alpha[i] + cov);
    }
    else
    {
        s.color[i] = {max_f(s.color[i].r, c.r * cov), max_f(s.color[i].g, c.g * cov),
                      max_f(s.color[i].b, c.b * cov)};
        s.alpha[i] = max_f(s.alpha[i], cov);
    }
}

// Per-LED coverage from signed distance d, for a falloff with inverse edge
// width inv_w (see coverage_from_distance).
template <Falloff F>
NEOTREE_INLINE float falloff_at(float d, float inv_w)
{
    if constexpr (F == Falloff::hard)
    {
        return d <= 0.0f ? 1.0f : 0.0f;
    }
    else if constexpr (F == Falloff::linear)
    {
        return clamp01(0.5f - d * inv_w);
    }
    else if constexpr (F == Falloff::smooth)
    {
        float s = clamp01(0.5f - d * inv_w);
        return s * s * (3.0f - 2.0f * s);
    }
    else   // glow
    {
        if (d <= 0.0f)
        {
            return 1.0f;
        }
        float k = d * inv_w;
        return k >= 2.0f ? 0.0f : std::exp(-2.0f * k * k);
    }
}

// Draws one entity, specialized by shape and falloff so it decides nothing
// per LED. Set up once per entity (the constructor); run() is the per-LED
// loop, called once for a height range or once per run of LEDs that
// LedGeometry::near yields - so the setup (a divide among it) isn't repeated
// per run.
template <EntityCombine Combine, Shape S, Falloff F>
struct ShapeDraw
{
    const LedGeometry &geometry;
    EntityScratch &scratch;
    Rgb c;
    float opacity;   // fades and dimming: how see-through, not how dark
    Vec3 axis;
    Vec3 pos;
    float size;
    float thickness;
    float half_length;
    float angle;
    float inv_w;
    // A sphere, shell or capsule reaches only part of what culling yields:
    // LEDs beyond its reach are rejected on squared distance, before the
    // square root and the falloff.
    float outer2;

    ShapeDraw(const Entity &e, Vec3 axis_, Rgb c_, float opacity_, const LedGeometry &g, EntityScratch &s)
        : geometry(g), scratch(s), c(c_), opacity(opacity_), axis(axis_), pos(e.pos), size(e.size), thickness(e.thickness),
          half_length(e.length), angle(e.angle), inv_w(1.0f / std::max(e.edge_mm, 1e-3f))
    {
        const float outer = size + (S == Shape::shell ? thickness : 0.0f) + falloff_reach(e);
        outer2 = outer * outer;
    }

    NEOTREE_INLINE uint32_t run(std::span<const uint16_t> leds) const
    {
        // Locals, not members: read through this, the compiler would reload
        // them after every store to the (float) scratch buffer, since it
        // can't rule out the two overlapping.
        const Vec3 p = pos;
        const Vec3 ax = axis;
        const Rgb col = c;
        const float op = opacity;
        const float sz = size;
        const float thick = thickness;
        const float half = half_length;
        const float ang = angle;
        const float iw = inv_w;
        const float o2 = outer2;
        const LedGeometry &g = geometry;
        EntityScratch &out = scratch;
        for (uint16_t i : leds)
        {
            float d;
            if constexpr (S == Shape::wedge)
            {
                float a = std::fabs(g.angle(i) - ang);
                a = min_f(a, two_pi - a);   // both angles are in [0, 2pi)
                d = (a - sz) * g.radius(i);
            }
            else
            {
                const Vec3 rel{g.x(i) - p.x, g.y(i) - p.y, g.z(i) - p.z};
                if constexpr (S == Shape::sphere)
                {
                    const float r2 = dot(rel, rel);
                    if (r2 > o2)
                    {
                        continue;
                    }
                    d = std::sqrt(r2) - sz;
                }
                else if constexpr (S == Shape::slab)
                {
                    d = std::fabs(dot(rel, ax)) - sz;
                }
                else if constexpr (S == Shape::shell)
                {
                    const float r2 = dot(rel, rel);
                    if (r2 > o2)
                    {
                        continue;
                    }
                    d = std::fabs(std::sqrt(r2) - sz) - thick;
                }
                else   // capsule
                {
                    float t = clamp_f(dot(rel, ax), -half, half);
                    Vec3 off = sub(rel, scale(ax, t));
                    const float r2 = dot(off, off);
                    if (r2 > o2)
                    {
                        continue;
                    }
                    d = std::sqrt(r2) - sz;
                }
            }
            float cov = falloff_at<F>(d, iw) * op;
            if (cov > 0.0f)
            {
                accumulate<Combine>(out, i, col, cov);
            }
        }
        return static_cast<uint32_t>(leds.size());
    }
};

// One entity with its shape and falloff known: round shapes (spheres,
// shells) test only the LEDs near them (LedGeometry::near); the rest, the
// LEDs in their height range, or all of them if unbounded in height.
template <EntityCombine Combine, Shape S, Falloff F>
NEOTREE_INLINE uint32_t draw_with(const Entity &e, Vec3 axis, Rgb c, float opacity, float reach,
                                  const LedGeometry &geometry, EntityScratch &scratch)
{
    const ShapeDraw<Combine, S, F> draw(e, axis, c, opacity, geometry, scratch);
    if constexpr (S == Shape::sphere || S == Shape::shell)
    {
        uint32_t n = 0;
        geometry.near(e.pos, reach, [&](std::span<const uint16_t> leds) { n += draw.run(leds); });
        return n;
    }
    else
    {
        return draw.run(reach < 0.0f ? geometry.z_order() : geometry.in_z_range(e.pos.z - reach, e.pos.z + reach));
    }
}

template <EntityCombine Combine, Shape S>
NEOTREE_INLINE uint32_t draw_falloff(const Entity &e, Vec3 axis, Rgb c, float opacity, float reach,
                                     const LedGeometry &geometry, EntityScratch &scratch)
{
    switch (e.falloff)
    {
    case Falloff::hard: return draw_with<Combine, S, Falloff::hard>(e, axis, c, opacity, reach, geometry, scratch);
    case Falloff::linear: return draw_with<Combine, S, Falloff::linear>(e, axis, c, opacity, reach, geometry, scratch);
    case Falloff::smooth: return draw_with<Combine, S, Falloff::smooth>(e, axis, c, opacity, reach, geometry, scratch);
    case Falloff::glow: return draw_with<Combine, S, Falloff::glow>(e, axis, c, opacity, reach, geometry, scratch);
    }
    return 0;
}

template <EntityCombine Combine>
NEOTREE_INLINE uint32_t draw_entity(const Entity &e, const LedGeometry &geometry, EntityScratch &scratch)
{
    const float k = e.brightness * life_fade(e);
    if (k <= 0.0f)
    {
        return 0;
    }
    // Fading in or out, and brightness below 1, make the entity see-through:
    // they scale its coverage, so what's below shows through. (Scaling its
    // color instead darkened it over whatever was below - toward black on a
    // colored scene - and the scene then snapped back when it went.)
    // Brightness above 1 still brightens.
    const float opacity = std::min(k, 1.0f);
    const float gain = std::max(k, 1.0f);
    const Rgb c{e.color.r * gain, e.color.g * gain, e.color.b * gain};
    // Only slabs and capsules have an axis; normalizing it costs a square
    // root and a divide per entity per frame.
    const bool has_axis = e.shape == Shape::slab || e.shape == Shape::capsule;
    const Vec3 axis = has_axis ? effective_axis(e) : Vec3{0.0f, 0.0f, 1.0f};
    const float reach = z_reach(e, axis);
    switch (e.shape)
    {
    case Shape::sphere: return draw_falloff<Combine, Shape::sphere>(e, axis, c, opacity, reach, geometry, scratch);
    case Shape::slab: return draw_falloff<Combine, Shape::slab>(e, axis, c, opacity, reach, geometry, scratch);
    case Shape::shell: return draw_falloff<Combine, Shape::shell>(e, axis, c, opacity, reach, geometry, scratch);
    case Shape::capsule: return draw_falloff<Combine, Shape::capsule>(e, axis, c, opacity, reach, geometry, scratch);
    case Shape::wedge: return draw_falloff<Combine, Shape::wedge>(e, axis, c, opacity, reach, geometry, scratch);
    }
    return 0;
}

}  // namespace

NEOTREE_HOT uint32_t render_entities(const EntityPool &pool, uint8_t slot, uint8_t layer, EntityCombine combine,
                                     const LedGeometry &geometry, EntityScratch &scratch)
{
    // Entities only ever light positioned LEDs, and only those are read back
    // (composite), so only those need clearing.
    for (uint16_t i : geometry.z_order())
    {
        scratch.color[i] = Rgb{};
        scratch.alpha[i] = 0.0f;
    }
    uint32_t evals = 0;
    for (uint16_t k = 0; k < pool.capacity; k++)
    {
        if (!pool.alive(k))
        {
            continue;
        }
        const Entity &e = pool.item(k);
        if (e.slot != slot || e.layer != layer)
        {
            continue;
        }
        evals += combine == EntityCombine::add ? draw_entity<EntityCombine::add>(e, geometry, scratch)
                                               : draw_entity<EntityCombine::max>(e, geometry, scratch);
    }
    return evals;
}

}  // namespace neotree
