#pragma once
// Direct control (docs/RENDERER.md 12, M7): entities a phone spawns and
// moves itself, in a slot whose mode offers them (ModeDef::direct - the
// "play" mode). There's no special path: they're ordinary entities in that
// slot, colliding and raising events like any other.
//
// Each belongs to its phone - owner 1-254 (0 is the slot's mode) - under the
// phone's own id, and they all go when the phone does (direct_release).
//   - A ball is spawned from the mode's template 0 with the phone's position,
//     velocity and color, then left to the physics. A phone has at most
//     max_direct_balls; another replaces its oldest.
//   - A brush is the mode's template 1, kinematic, moved by the phone's
//     samples. Its glow shows where it is; with the pen down it leaves
//     strokes through the mode's stroke hook. It lives only while samples
//     keep coming (brush_lease_s).

#include <cstdint>

#include "neotree/color.hpp"
#include "neotree/types.hpp"

namespace neotree {

class Engine;

enum class DirectKind : uint8_t
{
    ball = 0,
    brush = 1,
};

constexpr uint8_t max_direct_balls = 12;   // per phone
constexpr float brush_lease_s = 1.0f;      // a brush with no samples for this long fades out
constexpr uint8_t direct_all = 0xFF;       // direct_kill: every entity of the owner

struct BrushSample
{
    Vec3 pos;                  // mm
    Rgb color;
    float radius_mm = 80.0f;
    bool pen_down = false;     // leave a stroke from the previous sample to this one
};

// False if the slot's mode doesn't offer direct control, it has no template
// for this kind, or the slot's entity quota is full.
bool direct_spawn(Engine &engine, uint8_t slot, uint8_t owner, uint8_t id, DirectKind kind, Vec3 pos, Vec3 vel,
                  Rgb color, float size_mm);
// Moves (or creates) the owner's brush `id` in the slot.
bool direct_brush(Engine &engine, uint8_t slot, uint8_t owner, uint8_t id, const BrushSample &sample);
// Removes the owner's entity `id` (direct_all: all of them). Returns how many.
uint16_t direct_kill(Engine &engine, uint8_t owner, uint8_t id);
// Everything the owner had: the phone disconnected.
inline uint16_t direct_release(Engine &engine, uint8_t owner) { return direct_kill(engine, owner, direct_all); }
// Called by the engine each tick: a brush between samples coasts to a stop
// rather than flying off along its last velocity.
void direct_tick(Engine &engine);
// Entities owned by phones.
uint16_t direct_count(const Engine &engine);

}  // namespace neotree
