#pragma once
// The engine's top level: owns simulation time and (from M2 on) the scene,
// and turns them into one color per LED each frame. The platform - firmware
// frame loop or host simulator - decides how much time passes and when to
// render; the engine never reads a clock itself. See docs/RENDERER.md.

#include <cstdint>
#include <span>

#include "neotree/clock.hpp"
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

    // The scene and master settings are edited directly by the platform
    // between calls (single-threaded: the firmware applies commands on core0
    // between frames).
    Scene &scene() { return scene_; }
    const Scene &scene() const { return scene_; }
    MasterSettings &master() { return master_; }

    int64_t time_us() const { return clock_.time_us(); }
    const SimClock &clock() const { return clock_; }
    const EngineStats &stats() const { return stats_; }
    const LedGeometry &geometry() const { return *geometry_; }
    Rng &rng() { return rng_; }

private:
    void tick();
    void log(const char *fmt, ...) const;

    const LedGeometry *geometry_ = nullptr;
    EngineConfig config_{};
    LogFn log_ = nullptr;
    SimClock clock_{};
    Scene scene_{};
    MasterSettings master_{};
    Rng rng_{};
    EngineStats stats_{};
};

}  // namespace neotree
