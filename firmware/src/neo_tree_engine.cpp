#include "neo_tree_engine.hpp"

#include "pico/stdlib.h"

#include "neo_tree_config.hpp"
#include "neo_tree_event_log.hpp"
#include "neotree/engine.hpp"

namespace {

// pylon_geometry.py's "unmapped" sentinel z.
constexpr int16_t unmapped_z = -32768;

// Static: the geometry is ~31 KB and the frame 12 KB - far too big for
// core0's stack.
neotree::LedGeometry geometry;
neotree::Engine engine;
neotree::Rgb frame[neotree::LedGeometry::max_leds];

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
        if (c.string_position != i || c.coordinates.z == unmapped_z)
        {
            continue;   // left as PositionSource::none
        }
        geometry.set(i, neotree::led_point_from_cylindrical(c.coordinates.z, c.coordinates.radius,
                                                            c.coordinates.omega % 360));
    }
    geometry.finalize();
    stats.geometry_build_us = (uint32_t)(time_us_64() - t0);
    stats.leds = geometry.count();
    stats.positioned = geometry.positioned_count();
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
    last_frame_us = time_us_64();
}

void engine_host_reload_geometry()
{
    build_geometry();
}

void engine_host_frame(uint64_t now_us)
{
    uint64_t t0 = time_us_64();
    engine.advance((int64_t)(now_us - last_frame_us));
    last_frame_us = now_us;
    uint64_t t1 = time_us_64();
    engine.render(frame);
    uint64_t t2 = time_us_64();

    const neotree::EngineStats &es = engine.stats();
    stats.ticks = es.ticks;
    stats.ticks_dropped = es.ticks_dropped;
    stats.frames = es.frames;
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
    s.geometry_build_us = stats.geometry_build_us;
    s.leds = stats.leds;
    s.positioned = stats.positioned;
    return s;
}
