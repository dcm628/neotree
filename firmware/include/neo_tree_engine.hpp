#ifndef _NEO_TREE_ENGINE_HPP
#define _NEO_TREE_ENGINE_HPP

// Firmware host for the rendering engine (engine/, docs/RENDERER.md). Core0
// only (except engine_host_stats). The engine renders every LED frame; the
// base scene is the Canvas (docs/RENDERER.md 8.3), and the legacy color
// commands are translated into edits on its layers.

#include <cstddef>
#include <cstdint>

#include "dcm_rgb.hpp"

struct engine_host_stats_t
{
    uint64_t ticks;
    uint64_t ticks_dropped;
    uint64_t frames;
    uint32_t last_advance_us;
    uint32_t max_advance_us;
    uint32_t last_render_us;      // composite + master + packing
    uint32_t max_render_us;
    uint32_t max_advance_at_ms;   // uptime when each max happened
    uint32_t max_render_at_ms;
    uint32_t slow_frames;         // advance + render over engine_slow_frame_us
    uint32_t led_evals;           // last frame's LED x layer evaluations
    uint32_t geometry_build_us;
    uint16_t leds;
    uint16_t positioned;
    uint32_t rejected_edits;      // legacy commands whose target layer doesn't exist
    uint16_t entities;
    uint32_t spawns_failed;
    uint8_t demo;                 // neotree::Demo running in the demo slot (0 = none)
    // Behavior (rules, events, runaway limits) - see neotree/behavior.hpp.
    uint32_t rule_fires;
    uint32_t events_dropped;
    uint32_t actions_dropped;
    uint32_t spawns_over_quota;
    uint16_t peak_entities;
};

// A frame whose engine work takes longer than this is counted as slow.
constexpr uint32_t engine_slow_frame_us = 2000;

// Builds the LED geometry from the position config, starts the engine, and
// sets up the base scene (the Canvas, seeded with the boot pattern from
// init_my_tree()). Call after load_pos_config_from_flash() and
// RGB_LED_3D::initialize_from_config().
void engine_host_init();

// Rebuilds the geometry after the position config changes (CONFIG_RELOAD,
// RESET_POS_CONFIG_TO_DEFAULT). The scene carries on.
void engine_host_reload_geometry();

// Advances the engine to now_us and renders a frame into words, packed for
// led_output_prepare_frame(): one per LED, (r << 24) | (g << 16) | (b << 8).
void engine_host_frame(uint64_t now_us, uint32_t *words, size_t count);

// Runs a built-in demo (neotree::Demo id) in the demo slot above the Canvas;
// 0 stops it. Returns false for an unknown id.
bool engine_host_set_demo(uint8_t demo);

// Lights on/off (TREE_OUTPUT) - the master stage; the scene is untouched.
void engine_host_set_output(bool enabled);

// Legacy color commands -> Canvas edits. Same semantics as the pre-engine
// RGB_LED_3D handlers they replace: "paint" is the per-LED overlay,
// "background" what shows wherever nothing is painted.
void engine_host_canvas_fill(const all_led_update_t *msg);          // ALL_LED_UPDATE: paint all (black paints black)
void engine_host_canvas_base(const all_led_update_t *msg);          // ALL_LED_UPDATE_BASE: background
void engine_host_canvas_single(const single_led_update_t *msg);     // black clears the paint
void engine_host_canvas_group(const group_led_update_t *msg);       // black paints black
void engine_host_canvas_volume_cartesian(const set_volume_cartesian_t *msg);
void engine_host_canvas_volume_cylindrical(const set_volume_cylindrical_t *msg);

// Safe from either core (a torn read of a counter only affects one status
// report).
engine_host_stats_t engine_host_stats();

#endif
