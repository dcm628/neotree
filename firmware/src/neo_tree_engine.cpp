#include "neo_tree_engine.hpp"

#include <span>

#include "pico/stdlib.h"

#include "dcm_physics_math.hpp"
#include "neo_tree_config.hpp"
#include "neo_tree_event_log.hpp"
#include "neotree/demos.hpp"
#include "neotree/engine.hpp"

namespace {

using neotree::Rgba8;

// pylon_geometry.py's "unmapped" sentinel z.
constexpr int16_t unmapped_z = -32768;

// Where the legacy commands land: the Canvas in slot 0 of the base scene
// (docs/RENDERER.md 8.3). Background = what shows where nothing is painted
// (the old "base" color); paint = the per-LED overlay (the old "secondary").
constexpr uint8_t canvas_slot = 0;
constexpr uint8_t canvas_background = 0;
constexpr uint8_t canvas_paint = 1;
// Built-in demos draw over the Canvas from here.
constexpr uint8_t demo_slot = 1;
neotree::Demo demo = neotree::Demo::none;

// Static: the geometry is ~31 KB and the engine (pixel buffers, entities,
// its own float frame) ~95 KB - far too big for core0's stack.
neotree::LedGeometry geometry;
neotree::Engine engine;
neotree::Rgb8 frame[neotree::LedGeometry::max_leds];

// The integer positions the legacy volume commands test against, exactly as
// RGB_LED_3D holds them (refreshed at the same points: boot and config
// reloads), so region edges fall on the same LEDs as before.
cylindrical_coordinates legacy_positions[neotree::LedGeometry::max_leds];

uint64_t last_frame_us = 0;
volatile engine_host_stats_t stats = {};

void log_event(const char *text) { event_logf("%s", text); }

void build_geometry()
{
    uint64_t t0 = time_us_64();
    geometry.reset(max_led_config_size);
    for (uint16_t i = 0; i < max_led_config_size; i++)
    {
        string_led_config c = lookup_pos_config(i);
        if (c.string_position != i)
        {
            continue;   // left as PositionSource::none, legacy position unchanged
        }
        legacy_positions[i] = c.coordinates;
        if (c.coordinates.z == unmapped_z)
        {
            continue;
        }
        geometry.set(i, neotree::led_point_from_cylindrical(c.coordinates.z, c.coordinates.radius,
                                                            c.coordinates.omega % 360));
    }
    geometry.finalize();
    stats.geometry_build_us = (uint32_t)(time_us_64() - t0);
    stats.leds = geometry.count();
    stats.positioned = geometry.positioned_count();
}

void build_canvas()
{
    neotree::Scene &scene = engine.scene();
    scene.clear_slot(canvas_slot);
    int background = scene.add_layer(canvas_slot, neotree::LayerType::pixel);
    int paint = scene.add_layer(canvas_slot, neotree::LayerType::pixel);
    if (background != canvas_background || paint != canvas_paint)
    {
        event_logf("engine: canvas setup failed (%d, %d)", background, paint);
        return;
    }
    // Background starts as the boot pattern, fully opaque; paint starts
    // empty (transparent), as the old secondary colors started off.
    std::span<Rgba8> bg = scene.pixels(canvas_slot, canvas_background);
    for (size_t i = 0; i < RGB_LED_3D::string_vec.size() && i < bg.size(); i++)
    {
        dcm_rgb_data c = RGB_LED_3D::string_vec[i]->get_base_RGB();
        bg[i] = {c.bytes.red, c.bytes.green, c.bytes.blue, 255};
    }
}

// The Canvas layer a legacy command edits, or an empty span (command
// rejected) if the live scene has no such layer.
std::span<Rgba8> canvas_layer(uint8_t layer)
{
    std::span<Rgba8> px = engine.scene().pixels(canvas_slot, layer);
    if (px.empty())
    {
        stats.rejected_edits = stats.rejected_edits + 1;
    }
    return px.first(px.empty() ? 0 : geometry.count());
}

Rgba8 opaque(uint8_t r, uint8_t g, uint8_t b) { return {r, g, b, 255}; }

// Packs the rendered frame for led_output: (r << 24) | (g << 16) | (b << 8).
void pack_words(uint32_t *words, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        words[i] = ((uint32_t)frame[i].r << 24) | ((uint32_t)frame[i].g << 16) | ((uint32_t)frame[i].b << 8);
    }
}

}  // namespace

void engine_host_init()
{
    build_geometry();
    neotree::EngineConfig config;
    config.tick_hz = 120;
    config.seed = 1;
    config.max_ticks_per_advance = 8;
    engine.init(geometry, config, log_event);
    build_canvas();
    last_frame_us = time_us_64();
}

void engine_host_reload_geometry()
{
    build_geometry();
}

bool engine_host_set_demo(uint8_t id)
{
    if (id >= static_cast<uint8_t>(neotree::Demo::count))
    {
        return false;
    }
    demo = static_cast<neotree::Demo>(id);
    neotree::setup_demo(engine, demo, demo_slot);
    stats.demo = id;
    event_logf("demo: %s", neotree::demo_name(demo));
    return true;
}

void engine_host_set_output(bool enabled)
{
    engine.master().output_enabled = enabled;
}

void engine_host_frame(uint64_t now_us, uint32_t *words, size_t count)
{
    uint64_t t0 = time_us_64();
    engine.advance((int64_t)(now_us - last_frame_us));
    last_frame_us = now_us;
    uint64_t t1 = time_us_64();
    neotree::update_demo(engine, demo, demo_slot);
    engine.render_bytes(frame);
    size_t n = count < geometry.count() ? count : geometry.count();
    pack_words(words, n);
    for (size_t i = n; i < count; i++)
    {
        words[i] = 0;
    }
    uint64_t t2 = time_us_64();

    const neotree::EngineStats &es = engine.stats();
    stats.ticks = es.ticks;
    stats.ticks_dropped = es.ticks_dropped;
    stats.frames = es.frames;
    stats.led_evals = es.last_frame_led_evals;
    stats.entities = es.entities;
    stats.spawns_failed = es.spawns_failed;
    const neotree::BehaviorStats &bs = engine.behavior().stats();
    stats.rule_fires = bs.rule_fires;
    stats.events_dropped = bs.events_dropped;
    stats.actions_dropped = bs.actions_dropped;
    stats.spawns_over_quota = bs.spawns_over_quota;
    stats.peak_entities = bs.peak_entities;
    stats.last_advance_us = (uint32_t)(t1 - t0);
    stats.last_render_us = (uint32_t)(t2 - t1);
    uint32_t now_ms = (uint32_t)(t2 / 1000);
    if (stats.last_advance_us > stats.max_advance_us)
    {
        stats.max_advance_us = stats.last_advance_us;
        stats.max_advance_at_ms = now_ms;
    }
    if (stats.last_render_us > stats.max_render_us)
    {
        stats.max_render_us = stats.last_render_us;
        stats.max_render_at_ms = now_ms;
    }
    if (stats.last_advance_us + stats.last_render_us > engine_slow_frame_us)
    {
        stats.slow_frames = stats.slow_frames + 1;
    }
}

void engine_host_canvas_fill(const all_led_update_t *msg)
{
    for (Rgba8 &px : canvas_layer(canvas_paint))
    {
        px = opaque(msg->r, msg->g, msg->b);
    }
}

void engine_host_canvas_base(const all_led_update_t *msg)
{
    for (Rgba8 &px : canvas_layer(canvas_background))
    {
        px = opaque(msg->r, msg->g, msg->b);
    }
}

void engine_host_canvas_single(const single_led_update_t *msg)
{
    std::span<Rgba8> paint = canvas_layer(canvas_paint);
    if (msg->led_string_position >= paint.size())
    {
        return;
    }
    // Black clears the paint back to the background (the old
    // set_secondary_RGB clear_on_zero); the stored color is kept.
    Rgba8 &px = paint[msg->led_string_position];
    if (msg->r == 0 && msg->g == 0 && msg->b == 0)
    {
        px.a = 0;
    }
    else
    {
        px = opaque(msg->r, msg->g, msg->b);
    }
}

void engine_host_canvas_group(const group_led_update_t *msg)
{
    std::span<Rgba8> paint = canvas_layer(canvas_paint);
    uint8_t n = msg->count < max_group_update_entries ? msg->count : max_group_update_entries;
    for (uint8_t i = 0; i < n; i++)
    {
        const group_led_entry_t &e = msg->entries[i];
        if (e.led_string_position < paint.size())
        {
            paint[e.led_string_position] = opaque(e.r, e.g, e.b);
        }
    }
}

void engine_host_canvas_volume_cartesian(const set_volume_cartesian_t *msg)
{
    std::span<Rgba8> paint = canvas_layer(canvas_paint);
    for (size_t i = 0; i < paint.size(); i++)
    {
        // Same integer conversion and inclusive tests as the old handler.
        cartesian_coordinates pos = transform_cylindrical_to_cartesian(legacy_positions[i]);
        bool inside = (pos.x >= msg->x_min && pos.x <= msg->x_max) && (pos.y >= msg->y_min && pos.y <= msg->y_max) &&
                      (pos.z >= msg->z_min && pos.z <= msg->z_max);
        if (inside)
        {
            paint[i] = opaque(msg->r, msg->g, msg->b);
        }
        else if (msg->clear_outside_volume)
        {
            paint[i].a = 0;
        }
    }
}

void engine_host_canvas_volume_cylindrical(const set_volume_cylindrical_t *msg)
{
    std::span<Rgba8> paint = canvas_layer(canvas_paint);
    for (size_t i = 0; i < paint.size(); i++)
    {
        const cylindrical_coordinates &pos = legacy_positions[i];
        bool inside = (pos.z >= msg->z_min && pos.z <= msg->z_max) &&
                      (pos.radius >= msg->radius_min && pos.radius <= msg->radius_max) &&
                      (pos.omega >= msg->omega_min && pos.omega <= msg->omega_max);
        if (inside)
        {
            paint[i] = opaque(msg->r, msg->g, msg->b);
        }
        else if (msg->clear_outside_volume)
        {
            paint[i].a = 0;
        }
    }
}

engine_host_stats_t engine_host_stats()
{
    engine_host_stats_t s;
    s.ticks = stats.ticks;
    s.ticks_dropped = stats.ticks_dropped;
    s.frames = stats.frames;
    s.last_advance_us = stats.last_advance_us;
    s.max_advance_us = stats.max_advance_us;
    s.last_render_us = stats.last_render_us;
    s.max_render_us = stats.max_render_us;
    s.max_advance_at_ms = stats.max_advance_at_ms;
    s.max_render_at_ms = stats.max_render_at_ms;
    s.slow_frames = stats.slow_frames;
    s.led_evals = stats.led_evals;
    s.geometry_build_us = stats.geometry_build_us;
    s.leds = stats.leds;
    s.positioned = stats.positioned;
    s.rejected_edits = stats.rejected_edits;
    s.entities = stats.entities;
    s.spawns_failed = stats.spawns_failed;
    s.demo = stats.demo;
    s.rule_fires = stats.rule_fires;
    s.events_dropped = stats.events_dropped;
    s.actions_dropped = stats.actions_dropped;
    s.spawns_over_quota = stats.spawns_over_quota;
    s.peak_entities = stats.peak_entities;
    return s;
}
