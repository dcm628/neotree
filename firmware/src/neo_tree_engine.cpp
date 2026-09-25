#include "neo_tree_engine.hpp"

#include <cstring>
#include <span>

#include "pico/stdlib.h"

#include "dcm_physics_math.hpp"
#include "neo_tree_config.hpp"
#include "neo_tree_event_log.hpp"
#include "neo_tree_protocol.hpp"
#include "neo_tree_scene_store.hpp"
#include "hardware/sync.h"
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
// The old DEMO command's modes go here, over the Canvas.
constexpr uint8_t demo_slot = 1;

// The scene's state and the library as JSON, published by core0 for the
// status report and the pushes to apps (built on core1): snapshots under a
// lock, not live reads of the engine.
constexpr size_t scene_json_max = 2048;
char scene_json[scene_json_max] = "{}";
uint32_t scene_revision = 0;
constexpr size_t library_json_max = 2048;
char library_json[library_json_max] = "{}";
uint32_t library_revision = 0;
spin_lock_t *scene_lock = nullptr;
uint64_t scene_published_us = 0;

// Library changes are written to flash once they settle: a second after the
// last one (a flash write pauses both cores for tens of milliseconds).
constexpr uint64_t store_settle_us = 1'000'000;
uint32_t stored_revision = 0;
uint32_t pending_revision = 0;
uint64_t pending_since_us = 0;

// Scratch for the library commands - too big for core0's stack.
neotree::SceneSpec scratch_scene;
neotree::Show scratch_show;

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

// Mode commands (and DEMO) are queued and applied at the start of the next
// frame instead of inside process_msg: setting a mode up runs deep (director
// -> mode setup -> rules), and process_msg's own frame is large, so applying
// them there took core0 to ~3 KB of its 4 KB stack.
struct pending_command
{
    uint8_t bytes[engine_mode_command_max_len];
    uint8_t len;
};
constexpr uint8_t max_pending_commands = 8;
pending_command pending[max_pending_commands];
uint8_t pending_count = 0;

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

// EngineConfig::canvas_seed: the Canvas background starts as the boot
// pattern from init_my_tree(), as the old base colors did.
void seed_canvas(std::span<Rgba8> bg)
{
    for (size_t i = 0; i < RGB_LED_3D::string_vec.size() && i < bg.size(); i++)
    {
        dcm_rgb_data c = RGB_LED_3D::string_vec[i]->get_base_RGB();
        bg[i] = {c.bytes.red, c.bytes.green, c.bytes.blue, 255};
    }
}

void publish_scene()
{
    static char next[scene_json_max];   // static: keep it off core0's stack
    engine.director().describe_state(next, sizeof(next));
    const uint32_t rev = engine.director().revision();
    uint32_t irq = spin_lock_blocking(scene_lock);
    memcpy(scene_json, next, sizeof(scene_json));
    scene_revision = rev;
    spin_unlock(scene_lock, irq);
    scene_published_us = time_us_64();
}

void publish_library()
{
    static char next[library_json_max];
    const neotree::Library &lib = engine.director().library();
    lib.describe(next, sizeof(next));
    uint32_t irq = spin_lock_blocking(scene_lock);
    memcpy(library_json, next, sizeof(library_json));
    library_revision = lib.revision();
    spin_unlock(scene_lock, irq);
}

size_t copy_snapshot(const char *src, size_t src_max, char *out, size_t cap)
{
    if (scene_lock == nullptr || cap == 0)
    {
        return 0;
    }
    uint32_t irq = spin_lock_blocking(scene_lock);
    size_t n = strnlen(src, src_max);
    n = n < cap - 1 ? n : cap - 1;
    memcpy(out, src, n);
    out[n] = '\0';
    spin_unlock(scene_lock, irq);
    return n;
}

// Saves the library once changes have settled.
void store_when_settled(uint64_t now_us)
{
    const uint32_t rev = engine.director().library().revision();
    if (rev != pending_revision)
    {
        pending_revision = rev;
        pending_since_us = now_us;
    }
    if (pending_revision != stored_revision && now_us - pending_since_us >= store_settle_us)
    {
        scene_store_save(engine.director().library());
        stored_revision = pending_revision;   // a failed save is logged; retried on the next change
    }
}

float read_f32(const uint8_t *p)
{
    float f;
    memcpy(&f, p, sizeof(f));
    return f;
}

// The Canvas layer a legacy command edits, or an empty span (command
// rejected) if the live scene has no such layer.
std::span<Rgba8> canvas_layer(uint8_t layer)
{
    // Only while the Canvas mode is what's in its slot - otherwise the
    // legacy commands have no target and are rejected.
    const auto &info = engine.director().slot(canvas_slot);
    const bool canvas = info.state != neotree::SlotState::empty && info.spec.mode == neotree::find_mode("canvas");
    std::span<Rgba8> px = canvas ? engine.scene().pixels(canvas_slot, layer) : std::span<Rgba8>{};
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
    config.canvas_seed = seed_canvas;
    engine.init(geometry, config, log_event);
    // The stored library (user presets and shows, the base scene, the
    // startup show); the built-ins if there's none. Then the startup show,
    // or the base scene - the "Colors" preset (the Canvas alone) by default.
    neotree::Director &d = engine.director();
    scene_store_load(d.library());
    stored_revision = pending_revision = d.library().revision();
    const uint8_t boot_show = d.library().boot_show();
    if (boot_show == neotree::no_index || !d.play_show(engine, boot_show))
    {
        d.apply_scene(engine, d.library().base(), neotree::Transition::cut);
    }
    scene_lock = spin_lock_init(spin_lock_claim_unused(true));
    publish_scene();
    publish_library();
    last_frame_us = time_us_64();
}

void engine_host_reload_geometry()
{
    build_geometry();
}

static bool apply_demo(uint8_t id)
{
    // The old DEMO ids, now modes in the demo slot (0 = empty it).
    static const char *const ids[] = {"",       "layers", "lighthouse", "sweep",     "sweep", "sweep",
                                      "bounce", "snow",   "orbit",      "fireworks", "chain", "mixer"};
    if (id >= sizeof(ids) / sizeof(ids[0]))
    {
        return false;
    }
    neotree::SlotSpec spec = id == 0 ? neotree::SlotSpec{} : neotree::SlotSpec::of(ids[id]);
    if (id >= 3 && id <= 5)
    {
        spec.set("motion", static_cast<float>(id - 3));
    }
    engine.director().set_slot(engine, demo_slot, spec, neotree::Transition::cut);
    stats.demo = id;
    publish_scene();
    return true;
}

// SCENE_SAVE, LIBRARY_DELETE, SHOW_SET, SHOW_PLAY, SHOW_BOOT.
static bool apply_library_command(const uint8_t *msg)
{
    using namespace neotree;
    Director &d = engine.director();
    Library &lib = d.library();
    switch (static_cast<serial_msg_type>(msg[0]))
    {
    case serial_msg_type::SCENE_SAVE:
    {
        char name[name_size];
        if (msg[1] == 0)
        {
            d.capture_scene(scratch_scene, "Base");
            lib.set_base(scratch_scene);
            return true;
        }
        if (msg[1] != 1 || !copy_name(name, reinterpret_cast<const char *>(msg + 2), protocol_name_len))
        {
            return false;
        }
        d.capture_scene(scratch_scene, name);
        return lib.save_preset(scratch_scene, name) != no_index;
    }
    case serial_msg_type::LIBRARY_DELETE:
        switch (msg[1])
        {
        case 0: lib.reset_base(); return true;
        case 1: return lib.delete_preset(msg[2]);
        case 2: return lib.delete_show(msg[2]);
        default: return false;
        }
    case serial_msg_type::SHOW_SET:
    {
        const uint8_t count = msg[2];
        if (count > max_show_entries || count == 0)
        {
            return false;
        }
        clear_show(scratch_show);
        if (!copy_name(scratch_show.name, reinterpret_cast<const char *>(msg + 3), protocol_name_len))
        {
            return false;
        }
        scratch_show.loop = (msg[1] & 1) != 0;
        scratch_show.shuffle = (msg[1] & 2) != 0;
        const uint8_t *e = msg + 3 + protocol_name_len;
        for (uint8_t i = 0; i < count; i++, e += 3)
        {
            if (e[0] >= lib.preset_count())
            {
                return false;
            }
            memcpy(scratch_show.entries[i].preset, lib.preset(e[0]).name, name_size);
            scratch_show.entries[i].duration_s = static_cast<uint16_t>(e[1] | (e[2] << 8));
        }
        scratch_show.count = count;
        return lib.save_show(scratch_show) != no_index;
    }
    case serial_msg_type::SHOW_PLAY:
        if (msg[1] == no_index)
        {
            d.stop_show();
            return true;
        }
        return d.play_show(engine, msg[1]);
    case serial_msg_type::SHOW_BOOT:
        return lib.set_boot_show(msg[1]);
    default:
        return false;
    }
}

static bool apply_mode_command(const uint8_t *msg, size_t len)
{
    using namespace neotree;
    if (msg[0] == static_cast<uint8_t>(serial_msg_type::DEMO))
    {
        return apply_demo(msg[1]);
    }
    Director &d = engine.director();
    const uint8_t slot = len > 1 ? msg[1] : 0xFF;
    const bool slot_ok = slot < max_slots;
    bool ok = true;
    switch (static_cast<serial_msg_type>(msg[0]))
    {
    case serial_msg_type::SLOT_SET:
    {
        if (!slot_ok || (msg[2] != no_mode && mode_at(msg[2]) == nullptr))
        {
            return false;
        }
        SlotSpec spec;
        if (const ModeDef *def = mode_at(msg[2]))
        {
            spec.mode = msg[2];
            default_params(*def, spec.params);
        }
        d.set_slot(engine, slot, spec, msg[3] ? Transition::fade : Transition::cut);
        break;
    }
    case serial_msg_type::PARAM_SET:
    {
        ParamValue v;
        v.f = read_f32(msg + 3);
        v.c = to_rgb(msg[7], msg[8], msg[9]);
        ok = slot_ok && d.set_param(engine, slot, msg[2], v);
        break;
    }
    case serial_msg_type::SLOT_END:
        if (slot == 0xFF && msg[2] == 1)
        {
            // "Back to base" also ends a show - it's how to turn one off.
            d.stop_show();
            d.revert_scene(engine);
            break;
        }
        if (!slot_ok)
        {
            return false;
        }
        switch (msg[2])
        {
        case 0: d.end_slot(engine, slot, 0, EndReason::request); break;
        case 1: d.revert_slot(engine, slot); break;
        case 2: d.set_slot(engine, slot, SlotSpec{}, Transition::fade); break;
        case 3: d.restart_slot(engine, slot); break;
        default: return false;
        }
        break;
    case serial_msg_type::SLOT_LIFE:
    {
        if (!slot_ok || msg[6] > static_cast<uint8_t>(EndPolicy::hold))
        {
            return false;
        }
        Lifecycle life;
        life.duration_s = static_cast<float>(msg[2] | (msg[3] << 8));
        life.cycles = static_cast<uint16_t>(msg[4] | (msg[5] << 8));
        life.policy = static_cast<EndPolicy>(msg[6]);
        life.repeats = msg[7];
        life.transition = msg[9] ? Transition::fade : Transition::cut;
        life.transition_s = msg[10] * 0.1f;
        SlotSpec target;
        const SlotSpec *chain = nullptr;
        if (const ModeDef *def = mode_at(msg[8]))
        {
            target.mode = msg[8];
            default_params(*def, target.params);
            chain = &target;
        }
        d.set_lifecycle(slot, life, chain);
        break;
    }
    case serial_msg_type::INPUT:
        if (!slot_ok)
        {
            return false;
        }
        engine.input(slot, msg[2], read_f32(msg + 3));
        break;
    case serial_msg_type::PRESET:
        if (msg[1] >= d.library().preset_count())
        {
            return false;
        }
        d.apply_scene(engine, d.library().preset(msg[1]));
        break;
    case serial_msg_type::SCENE_SAVE:
    case serial_msg_type::LIBRARY_DELETE:
    case serial_msg_type::SHOW_SET:
    case serial_msg_type::SHOW_PLAY:
    case serial_msg_type::SHOW_BOOT:
        ok = apply_library_command(msg);
        break;
    default:
        return false;
    }
    publish_scene();
    return ok;
}

static bool queue_command(const uint8_t *msg, size_t len)
{
    if (len > sizeof(pending[0].bytes) || pending_count >= max_pending_commands)
    {
        return false;
    }
    memcpy(pending[pending_count].bytes, msg, len);
    pending[pending_count].len = static_cast<uint8_t>(len);
    pending_count++;
    return true;
}

bool engine_host_set_demo(uint8_t id)
{
    const uint8_t msg[2] = {static_cast<uint8_t>(serial_msg_type::DEMO), id};
    return queue_command(msg, sizeof(msg));
}

bool engine_host_mode_command(const uint8_t *msg, size_t len)
{
    return queue_command(msg, len);
}

size_t engine_host_scene_json(char *out, size_t cap)
{
    return copy_snapshot(scene_json, scene_json_max, out, cap);
}

uint32_t engine_host_scene_revision()
{
    return scene_revision;   // one aligned word: no lock needed to read it
}

size_t engine_host_library_json(char *out, size_t cap)
{
    return copy_snapshot(library_json, library_json_max, out, cap);
}

uint32_t engine_host_library_revision()
{
    return library_revision;
}

void engine_host_set_output(bool enabled)
{
    engine.master().output_enabled = enabled;
}

void engine_host_frame(uint64_t now_us, uint32_t *words, size_t count)
{
    for (uint8_t i = 0; i < pending_count; i++)
    {
        if (!apply_mode_command(pending[i].bytes, pending[i].len))
        {
            event_logf("mode command %u rejected", (unsigned)pending[i].bytes[0]);
        }
    }
    pending_count = 0;

    uint64_t t0 = time_us_64();
    engine.advance((int64_t)(now_us - last_frame_us));
    last_frame_us = now_us;
    uint64_t t1 = time_us_64();
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
    // Republish the scene state when it changes (apps are pushed it), and a
    // few times a second anyway for the ages in the status report.
    if (engine.director().revision() != scene_revision || t2 - scene_published_us > 250'000)
    {
        publish_scene();
    }
    if (engine.director().library().revision() != library_revision)
    {
        publish_library();
    }
    store_when_settled(t2);
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
