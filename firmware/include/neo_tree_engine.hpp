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

// A frame whose engine work takes longer than one frame at the target rate
// is counted as slow: it can't keep up. (It was 2 ms from M1, when the
// engine drew nothing; real scenes take 3-10 ms, so every frame counted.)
constexpr uint32_t engine_slow_frame_us = 1'000'000 / NEOTREE_FRAME_RATE;

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

// The old DEMO command: runs one of the former demos (now modes) in slot 1,
// above the Canvas; 0 empties it. Returns false for an unknown id.
bool engine_host_set_demo(uint8_t demo);

// SLOT_SET, PARAM_SET, SLOT_END, SLOT_LIFE, INPUT, PRESET and the library
// commands (see neo_tree_protocol.hpp), at most engine_mode_command_max_len bytes (a
// zero-padded message may be passed with that length). The command is queued
// and applied at the start of the next frame; returns false if the queue is
// full or the message too long. Invalid commands are logged when applied.
constexpr size_t engine_mode_command_max_len = 72;   // SHOW_SET with 16 entries: 71
bool engine_host_mode_command(const uint8_t *msg, size_t len);

// The scene's state as JSON (director describe_state), as last published by
// core0. Safe from either core. Returns the length written.
size_t engine_host_scene_json(char *out, size_t cap);
// The published scene's revision: changes when what's running changes (not
// as time passes) - when to push it to apps.
uint32_t engine_host_scene_revision();

// The library as JSON (Library::describe) and its revision, as last
// published by core0. Safe from either core.
size_t engine_host_library_json(char *out, size_t cap);
uint32_t engine_host_library_revision();

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
