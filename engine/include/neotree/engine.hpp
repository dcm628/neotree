#pragma once
// The engine's top level: owns simulation time and (from M2 on) the scene,
// and turns them into one color per LED each frame. The platform - firmware
// frame loop or host simulator - decides how much time passes and when to
// render; the engine never reads a clock itself. See docs/RENDERER.md.

#include <cstdint>
#include <span>

#include "neotree/behavior.hpp"
#include "neotree/clock.hpp"
#include "neotree/director.hpp"
#include "neotree/geometry.hpp"
#include "neotree/random.hpp"
#include "neotree/scene.hpp"
#include "neotree/types.hpp"

namespace neotree {

// Platform hook for diagnostic text (firmware: printf/event log; host: stderr).
using LogFn = void (*)(const char *text);

struct EngineConfig
{
    uint32_t tick_hz = 120;
    uint32_t seed = 1;
    // Upper bound on ticks run by one advance() call; time beyond it is
    // dropped (and counted) instead of run in a burst. The firmware keeps
    // this small so a stall can't snowball; the host simulator sets 0
    // (unlimited) to run far faster than real time.
    uint32_t max_ticks_per_advance = 8;
    // Fills the Canvas mode's background when it's set up (the firmware
    // seeds it with the boot pattern). Optional.
    void (*canvas_seed)(std::span<Rgba8> background) = nullptr;
    // Optional: a fast counter (the Pico's CPU cycle counter) for the
    // engine's profile of where its time goes (EngineProfile). Unset,
    // profiling costs nothing.
    uint32_t (*profile_clock)() = nullptr;
};

// Counts of EngineConfig::profile_clock spent in each part of the engine's
// work, summed since reset_profile(). Parts of a tick: the director (mode
// ticks, lifecycles), rule events, emitters, the entity loop (of which step:
// the motion itself), collisions and group counts. Parts of a render:
// composite (of which entity_draw: entities into their scratch buffers), then
// master (brightness and conversion to bytes).
struct EngineProfile
{
    uint32_t ticks = 0;
    uint32_t frames = 0;
    uint64_t director = 0;
    uint64_t events = 0;
    uint64_t emitters = 0;
    uint64_t entity_loop = 0;
    uint64_t step = 0;
    uint64_t collide = 0;
    uint64_t recount = 0;
    uint64_t composite = 0;
    uint64_t entity_draw = 0;
    uint64_t master = 0;
};

// The last stage before output (docs/RENDERER.md 5.2). Controls output, not
// scene content.
struct MasterSettings
{
    bool output_enabled = true;   // lights on/off: off renders all black, the scene is untouched
    float brightness = 1.0f;      // scales everything
    // Output curve: out = in ^ gamma. 1 = none, which keeps today's
    // behavior of sending commanded bytes unchanged.
    float gamma = 1.0f;
};

struct EngineStats
{
    uint16_t entities = 0;          // live entities
    uint32_t spawns_failed = 0;     // spawn() with the pool full
    uint64_t ticks = 0;             // ticks run
    uint64_t ticks_dropped = 0;     // ticks skipped by max_ticks_per_advance
    uint64_t frames = 0;            // render() calls
    // Work done by the last render, a platform-independent cost measure
    // (docs/RENDERER.md 11.4): LED x layer evaluations that reached a blend.
    uint32_t last_frame_led_evals = 0;
};

class Engine
{
public:
    // geometry must outlive the engine and be finalized.
    void init(const LedGeometry &geometry, const EngineConfig &config = {}, LogFn log = nullptr);

    // Lets dt_us of simulation time pass, running whole ticks as they come
    // due. Returns the number of ticks run.
    uint32_t advance(int64_t dt_us);

    // Runs exactly n ticks, independent of advance()'s pending time (single
    // stepping in the viewer, tests).
    void run_ticks(uint32_t n);

    // Writes one final color per LED, each channel 0..1 after the master
    // stage (out.size() should be geometry.count(); extra entries are left
    // alone). Rendering never changes the simulation, so it can run at any
    // rate relative to ticks - or be skipped.
    void render(std::span<Rgb> out);

    // The same frame, finished straight to 8-bit color in one pass (master
    // stage + rounding fused) - what a device sends to its LEDs. Uses an
    // engine-owned float buffer for the composite.
    void render_bytes(std::span<Rgb8> out);

    // The scene, entities, forces and master settings are edited directly by
    // the platform between calls (single-threaded: the firmware applies
    // commands on core0 between frames).
    Scene &scene() { return scene_; }
    const Scene &scene() const { return scene_; }
    MasterSettings &master() { return master_; }
    Forces &forces() { return forces_; }
    World &world() { return world_; }

    // Adds an entity (its current pos/vel become its respawn point). Returns
    // an invalid handle, and counts it, if the pool is full.
    Handle spawn(const Entity &entity);
    Entity *entity(Handle h) { return entities_.get(h); }
    bool destroy(Handle h) { return entities_.destroy(h); }
    // Removes every entity drawn into slot (all slots if slot < 0).
    void destroy_entities_in_slot(int slot);
    const EntityPool &entities() const { return entities_; }

    // Modes in slots, lifecycles and scenes (neotree/director.hpp).
    Director &director() { return director_; }
    const Director &director() const { return director_; }
    const EngineConfig &config() const { return config_; }
    // Diagnostic text through the platform's log hook (printf-style).
    void note(const char *fmt, ...) const;

    // Collisions, rules, templates and emitters (neotree/behavior.hpp).
    Behavior &behavior() { return behavior_; }
    const Behavior &behavior() const { return behavior_; }
    // Raises an input event (e.g. an app button) for slot's rules, handled
    // next tick.
    void input(uint8_t slot, uint8_t id, float value = 0.0f);

    int64_t time_us() const { return clock_.time_us(); }
    const SimClock &clock() const { return clock_; }
    const EngineStats &stats() const { return stats_; }
    const EngineProfile &profile() const { return profile_; }
    void reset_profile() { profile_ = {}; }
    const LedGeometry &geometry() const { return *geometry_; }
    Rng &rng() { return rng_; }

    // The Canvas's pixels (background and paint), kept while no Canvas is
    // in the scene, so the Home page's colors come back with it - after a
    // show step or a preset without it - instead of the boot pattern.
    struct CanvasMemory
    {
        bool saved = false;
        Rgba8 background[LedGeometry::max_leds];
        Rgba8 paint[LedGeometry::max_leds];
    };
    CanvasMemory &canvas_memory() { return canvas_memory_; }

private:
    void tick();
    void log(const char *fmt, ...) const;

    const LedGeometry *geometry_ = nullptr;
    EngineConfig config_{};
    LogFn log_ = nullptr;
    SimClock clock_{};
    Scene scene_{};
    MasterSettings master_{};
    EntityPool entities_{};
    Behavior behavior_{};
    Director director_{};
    EntityScratch scratch_{};
    Rgb frame_[LedGeometry::max_leds] = {};   // render_bytes' composite
    Forces forces_{};
    World world_{};
    Rng rng_{};
    EngineStats stats_{};
    CanvasMemory canvas_memory_{};
    EngineProfile profile_{};
};

}  // namespace neotree
