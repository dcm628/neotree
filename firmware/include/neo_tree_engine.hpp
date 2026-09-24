#ifndef _NEO_TREE_ENGINE_HPP
#define _NEO_TREE_ENGINE_HPP

// Firmware host for the rendering engine (engine/, docs/RENDERER.md). Core0
// only. M1: the engine runs every frame on the real LED positions, but its
// output isn't sent to the LEDs yet - this measures its cost on the Pico and
// proves the build. M2 makes it the output.

#include <cstdint>

// A frame whose engine work takes longer than this is counted as slow.
constexpr uint32_t engine_slow_frame_us = 2000;

struct engine_host_stats_t
{
    uint64_t ticks;
    uint64_t ticks_dropped;
    uint64_t frames;
    uint32_t last_advance_us;
    uint32_t max_advance_us;
    uint32_t last_render_us;
    uint32_t max_render_us;
    uint32_t max_advance_at_ms;   // uptime when each max happened
    uint32_t max_render_at_ms;
    uint32_t slow_frames;         // advance + render over engine_slow_frame_us
    uint32_t geometry_build_us;
    uint16_t leds;
    uint16_t positioned;
};

// Builds the LED geometry from the position config and starts the engine.
// Call after load_pos_config_from_flash().
void engine_host_init();

// Rebuilds the geometry after the position config changes (CONFIG_RELOAD,
// RESET_POS_CONFIG_TO_DEFAULT). Simulation state carries on.
void engine_host_reload_geometry();

// Advances the engine to now and renders a frame. Call once per LED frame.
void engine_host_frame(uint64_t now_us);

// Safe from either core (a torn read of a counter only affects one status
// report).
engine_host_stats_t engine_host_stats();

#endif
