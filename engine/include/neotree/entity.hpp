#pragma once
// Entities: simulated shapes that move, respond to forces, and light the LEDs
// they overlap (docs/RENDERER.md section 6). Each is drawn into an entity
// layer, addressed by (slot, layer).

#include <cstdint>

#include "neotree/geometry.hpp"
#include "neotree/pool.hpp"
#include "neotree/types.hpp"

namespace neotree {

constexpr uint16_t max_entities = 256;

// Collision groups (docs/RENDERER.md 6.5): an entity is in at most one.
constexpr uint8_t max_groups = 16;
constexpr uint8_t no_group = 0xFF;   // takes part in no collisions

enum class Shape : uint8_t
{
    sphere,    // ball, spark, snowflake: radius = size
    slab,      // plane of half-thickness size, normal = axis - today's sweeps
    shell,     // sphere surface of radius size, half wall thickness = thickness
    capsule,   // segment pos +- axis * length, radius size - streaks, comets
    wedge,     // slice around the trunk centred on angle, half-angle size (radians), full height
};

// How brightness falls off across the shape's edge (width edge_mm).
enum class Falloff : uint8_t
{
    hard,     // on inside, off outside
    linear,   // ramps across edge_mm, centred on the surface
    smooth,   // same span, eased
    glow,     // full inside, a long soft tail outside (~2 x edge_mm)
};

// What happens when an entity crosses a boundary.
enum class Bound : uint8_t
{
    pass,      // nothing
    stop,      // held at the boundary, outward velocity removed
    bounce,    // reflected, scaled by restitution
    destroy,   // removed
    respawn,   // back to where and how it was spawned
    wrap,      // floor <-> ceiling (outer: treated as stop)
};

enum class Surface : uint8_t
{
    none,
    envelope,   // held on the tree's outer envelope (plus surface_offset_mm)
};

struct Entity
{
    // Where it draws: an entity layer (slot, layer index).
    uint8_t slot = 1;
    uint8_t layer = 0;

    // Shape
    Shape shape = Shape::sphere;
    float size = 50.0f;
    float thickness = 20.0f;
    float length = 0.0f;
    Vec3 axis{0.0f, 0.0f, 1.0f};     // unit; slab normal, capsule direction
    bool align_to_velocity = false;  // capsule/slab axis follows the velocity
    float angle = 0.0f;              // wedge centre, radians
    float spin = 0.0f;               // wedge angular velocity, rad/s (+ = counter-clockwise from above)

    // Appearance
    Rgb color{1.0f, 1.0f, 1.0f};
    float brightness = 1.0f;
    Falloff falloff = Falloff::smooth;
    float edge_mm = 40.0f;
    float fade_in_s = 0.0f;          // from spawn
    float fade_out_s = 0.0f;         // before lifetime ends

    // Motion
    Vec3 pos{};
    Vec3 vel{};                      // mm/s
    float drag = 0.0f;               // per second: velocity decays by e^(-drag t)
    float gravity_scale = 1.0f;      // response to each global force
    float wind_scale = 1.0f;
    float swirl_scale = 1.0f;
    float restitution = 0.6f;        // bounce energy kept
    float mass = 1.0f;               // for collisions between entities
    bool kinematic = false;          // ignores forces (moved by commands); velocity still applies
    Surface surface = Surface::none;
    float surface_offset_mm = 0.0f;

    // Boundaries (world floor/ceiling heights and the tree's outer envelope)
    Bound floor = Bound::pass;
    Bound ceiling = Bound::pass;
    Bound outer = Bound::pass;

    // Life
    float age_s = 0.0f;
    float lifetime_s = 0.0f;         // 0 = forever
    Vec3 spawn_pos{};                // set by Engine::spawn; respawn returns here
    Vec3 spawn_vel{};

    // Interaction
    uint8_t group = no_group;        // collision group, 0..max_groups-1
    float collide_radius = 0.0f;     // 0 = from the shape (sphere: size, shell: size + thickness, capsule: size + length)

    // Direct control (neotree/direct.hpp): 0 = the slot's mode; otherwise
    // the phone (or other client) that spawned it, under its own id.
    uint8_t owner = 0;
    // The slot template it was copied from (index + 1; 0 = none), so edits to
    // a template can reach what was made from it (custom effects, live).
    uint8_t tmpl = 0;
    uint8_t direct_id = 0;
    uint8_t direct_kind = 0;         // DirectKind
    bool pen_down = false;           // a brush's last sample
};

using EntityPool = Pool<Entity, max_entities>;

// Global external influences (docs/RENDERER.md 6.4), scaled per entity.
struct Forces
{
    Vec3 gravity{0.0f, 0.0f, -9810.0f};   // mm/s^2 (real gravity by default)
    Vec3 wind{};                           // mm/s^2
    // Swirl around the trunk: tangential speed is pulled toward
    // swirl_rad_s x radius at swirl_rate per second, with the centripetal
    // pull that keeps a swirling entity on its circle.
    float swirl_rad_s = 0.0f;
    float swirl_rate = 0.0f;
};

// The space entities live in. Set from the LED geometry at init.
struct World
{
    float floor_z = 0.0f;
    float ceiling_z = 2000.0f;
    float outer_margin_mm = 0.0f;   // the outer bound is the envelope + this
};

// Which boundaries a step touched (StepResult::hits).
constexpr uint8_t hit_floor = 1;
constexpr uint8_t hit_ceiling = 2;
constexpr uint8_t hit_outer = 4;

struct StepResult
{
    bool alive = true;     // false: destroy it (by a boundary or its lifetime)
    bool expired = false;  // its lifetime ran out
    uint8_t hits = 0;      // hit_* bits for boundaries it crossed this step
};

// Advances one entity by dt.
StepResult step_entity(Entity &e, const Forces &forces, const World &world, const LedGeometry &geometry, float dt);

// The radius used for entity-entity collisions (0 = doesn't collide).
float collision_radius(const Entity &e);

// Premultiplied color + coverage accumulated from an entity layer's entities.
struct EntityScratch
{
    Rgb color[LedGeometry::max_leds];
    float alpha[LedGeometry::max_leds];
};

enum class EntityCombine : uint8_t
{
    add,   // overlapping entities add up (light)
    max,   // the brightest wins
};

// Draws every live entity assigned to (slot, layer) into scratch (cleared
// first). Returns LED x entity evaluations.
uint32_t render_entities(const EntityPool &pool, uint8_t slot, uint8_t layer, EntityCombine combine,
                         const LedGeometry &geometry, EntityScratch &scratch);

// Coverage of an entity at point p (0..1), exposed for tests.
float entity_coverage(const Entity &e, Vec3 p, float led_radius, float led_angle);

}  // namespace neotree
