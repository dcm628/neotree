#include "neo_tree_engine.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <span>

#include "pico/multicore.h"
#include "pico/stdlib.h"

#include "dcm_physics_math.hpp"
#include "neo_tree_clock.hpp"
#include "neo_tree_config.hpp"
#include "neo_tree_event_log.hpp"
#include "neo_tree_protocol.hpp"
#include "neo_tree_scene_store.hpp"
#include "hardware/structs/m33.h"
#include "hardware/sync.h"
#include "neo_tree_net_server.hpp"
#include "neotree/direct.hpp"
#include "neotree/effect.hpp"
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
constexpr size_t library_json_max = 3072;
char library_json[library_json_max] = "{}";
constexpr size_t schedule_json_max = 3072;
char schedule_json[schedule_json_max] = "{}";
uint32_t library_revision = 0;
spin_lock_t *scene_lock = nullptr;
uint64_t scene_published_us = 0;

// Library changes are written to flash once they settle: a second after the
// last one (a flash write pauses both cores for tens of milliseconds).
constexpr uint64_t store_settle_us = 1'000'000;
uint32_t stored_revision = 0;
uint32_t pending_revision = 0;
uint64_t pending_since_us = 0;
uint32_t stored_scenes_revision = 0;    // what's in each flash region
uint32_t stored_effects_revision = 0;

// FX_GET: the sections of the draft effect each owner asked for (bits), and
// one reply at a time for core1 to send (fx_reply_owner != 0: waiting; core0
// writes the reply only while it's 0).
uint8_t fx_wanted[engine_host_owner_usb + 1];
char fx_reply[net_reply_json_max];
size_t fx_reply_len = 0;
volatile uint8_t fx_reply_owner = 0;

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
    uint8_t owner;
};
constexpr uint8_t max_pending_commands = 8;
pending_command pending[max_pending_commands];
uint8_t pending_count = 0;

// The UDP stream's brush samples: the newest per owner and brush id (0-3),
// written by core1, taken by core0 each frame. Under stream_lock.
constexpr uint8_t stream_owners = engine_host_owner_usb + 1;
constexpr uint8_t stream_brushes = 4;
struct stream_sample
{
    bool fresh;
    uint8_t msg[brush_len];
};
stream_sample stream_box[stream_owners][stream_brushes];
spin_lock_t *stream_lock = nullptr;

void log_event(const char *text) { event_logf("%s", text); }

// EngineConfig::profile_clock: core0's CPU cycle counter (150 per us).
uint32_t cycle_clock() { return m33_hw->dwt_cyccnt; }

void enable_cycle_counter()
{
    m33_hw->demcr |= M33_DEMCR_TRCENA_BITS;
    m33_hw->dwt_cyccnt = 0;
    m33_hw->dwt_ctrl |= M33_DWT_CTRL_CYCCNTENA_BITS;
}

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
    // The schedule: built straight into its snapshot, under the lock (a
    // few hundred microseconds, only when the library changes).
    lib.describe_schedule(schedule_json, sizeof(schedule_json));
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
        // Only the regions that changed: a flash write pauses both cores.
        const neotree::Library &lib = engine.director().library();
        if (lib.effects_revision() != stored_effects_revision)
        {
            scene_store_save_effects(lib);
            stored_effects_revision = lib.effects_revision();
        }
        if (lib.scenes_revision() != stored_scenes_revision)
        {
            scene_store_save(lib);
            stored_scenes_revision = lib.scenes_revision();
        }
        stored_revision = pending_revision;   // a failed save is logged; retried on the next change
    }
}

// FX_GET: builds the next reply someone is waiting for, if core1 has sent
// the last one. USB's are printed.
void pump_fx_replies()
{
    if (fx_reply_owner != 0)
    {
        return;
    }
    for (uint8_t owner = 1; owner <= engine_host_owner_usb; owner++)
    {
        if (fx_wanted[owner] == 0)
        {
            continue;
        }
        uint8_t section = 0;
        while ((fx_wanted[owner] & (1u << section)) == 0)
        {
            section++;
        }
        fx_wanted[owner] = static_cast<uint8_t>(fx_wanted[owner] & ~(1u << section));
        fx_reply_len = neotree::effect_section_json(engine.director().draft(),
                                                   static_cast<neotree::EffectSection>(section), fx_reply,
                                                   sizeof(fx_reply));
        if (owner == engine_host_owner_usb)
        {
            printf("fx: %s\n", fx_reply);
            continue;
        }
        __dmb();   // the reply before the flag core1 reads
        fx_reply_owner = owner;
        return;
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
    enable_cycle_counter();
    config.profile_clock = cycle_clock;   // a few register reads per tick: always on
    engine.init(geometry, config, log_event);
    // The stored library (user presets and shows, the base scene, the
    // startup show); the built-ins if there's none. Then the startup show,
    // or the base scene - the "Colors" preset (the Canvas alone) by default.
    neotree::Director &d = engine.director();
    scene_store_load(d.library());
    stored_revision = pending_revision = d.library().revision();
    stored_scenes_revision = d.library().scenes_revision();
    stored_effects_revision = d.library().effects_revision();
    const uint8_t boot_show = d.library().boot_show();
    if (boot_show == neotree::no_index || !d.play_show(engine, boot_show))
    {
        d.apply_scene(engine, d.library().base(), neotree::Transition::cut);
    }
    scene_lock = spin_lock_init(spin_lock_claim_unused(true));
    stream_lock = spin_lock_init(spin_lock_claim_unused(true));
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

// BENCH: the same work timed with core1 running and paused, to see how much
// sharing the flash cache and memory with core1 (WiFi) costs core0.
static volatile float bench_sink;

static uint32_t bench_render(int n)
{
    uint64_t t0 = time_us_64();
    for (int i = 0; i < n; i++)
    {
        engine.render_bytes(frame);
    }
    return (uint32_t)((time_us_64() - t0) / n);
}

static uint32_t bench_ticks(int n)
{
    uint64_t t0 = time_us_64();
    engine.run_ticks(n);
    return (uint32_t)((time_us_64() - t0) / n);
}

// ns per call of each math function (1000 calls; inputs vary so nothing folds).
static void bench_math(uint32_t out_ns[4])
{
    float acc = 0.0f;
    uint64_t t0 = time_us_64();
    for (int i = 0; i < 1000; i++) acc += expf(-0.3f * (float)i * 1e-3f);
    uint64_t t1 = time_us_64();
    for (int i = 0; i < 1000; i++) acc += fmodf((float)i * 0.37f, 6.2831853f);
    uint64_t t2 = time_us_64();
    for (int i = 0; i < 1000; i++) acc += sqrtf((float)i + 0.5f);
    uint64_t t3 = time_us_64();
    for (int i = 0; i < 1000; i++) acc += atan2f((float)i - 500.0f, 250.0f);
    uint64_t t4 = time_us_64();
    bench_sink = acc;
    out_ns[0] = (uint32_t)(t1 - t0);   // us per 1000 = ns per call
    out_ns[1] = (uint32_t)(t2 - t1);
    out_ns[2] = (uint32_t)(t3 - t2);
    out_ns[3] = (uint32_t)(t4 - t3);
}

// The engine's own profile over 20 ticks and 10 renders, in cycles per tick /
// per frame (core1 running as usual).
static void bench_profile()
{
    engine.reset_profile();
    engine.run_ticks(20);
    for (int i = 0; i < 10; i++)
    {
        engine.render_bytes(frame);
    }
    const neotree::EngineProfile &p = engine.profile();
    auto per = [](uint64_t v, uint32_t n) { return (unsigned)(n != 0 ? v / n : 0); };
    event_logf("prof/tick: dir %u ev %u emit %u loop %u", per(p.director, p.ticks), per(p.events, p.ticks),
               per(p.emitters, p.ticks), per(p.entity_loop, p.ticks));
    event_logf("prof/tick: step %u coll %u recount %u", per(p.step, p.ticks), per(p.collide, p.ticks),
               per(p.recount, p.ticks));
    event_logf("prof/frame: comp %u draw %u master %u", per(p.composite, p.frames), per(p.entity_draw, p.frames),
               per(p.master, p.frames));
}

static void run_bench()
{
    uint32_t math_on[4], math_off[4];
    bench_profile();
    const uint32_t render_on = bench_render(10);
    const uint32_t ticks_on = bench_ticks(10);
    bench_math(math_on);
    multicore_lockout_start_blocking();
    const uint32_t render_off = bench_render(10);
    const uint32_t ticks_off = bench_ticks(10);
    bench_math(math_off);
    multicore_lockout_end_blocking();
    // One short line each: event log entries hold 72 characters.
    event_logf("bench: %u ent %u evals", (unsigned)engine.stats().entities,
               (unsigned)engine.stats().last_frame_led_evals);
    event_logf("bench: render %u us, core1 paused %u", (unsigned)render_on, (unsigned)render_off);
    event_logf("bench: tick %u us, core1 paused %u", (unsigned)ticks_on, (unsigned)ticks_off);
    event_logf("bench ns: expf %u/%u fmodf %u/%u", (unsigned)math_on[0], (unsigned)math_off[0],
               (unsigned)math_on[1], (unsigned)math_off[1]);
    event_logf("bench ns: sqrtf %u/%u atan2f %u/%u", (unsigned)math_on[2], (unsigned)math_off[2],
               (unsigned)math_on[3], (unsigned)math_off[3]);
}

static int16_t read_i16(const uint8_t *p) { return static_cast<int16_t>(p[0] | (p[1] << 8)); }

static neotree::Vec3 read_vec(const uint8_t *p)
{
    return {static_cast<float>(read_i16(p)), static_cast<float>(read_i16(p + 2)), static_cast<float>(read_i16(p + 4))};
}

// BRUSH: [40][slot][id][flags][x][y][z][r][g][b][radius]
static bool apply_brush(const uint8_t *msg, uint8_t owner)
{
    neotree::BrushSample s;
    s.pen_down = (msg[3] & 1) != 0;
    s.pos = read_vec(msg + 4);
    s.color = neotree::to_rgb(msg[10], msg[11], msg[12]);
    s.radius_mm = msg[13] != 0 ? static_cast<float>(msg[13]) : 80.0f;
    return neotree::direct_brush(engine, msg[1], owner, msg[2], s);
}

// ENTITY_SPAWN, ENTITY_KILL, BRUSH.
static bool apply_direct_command(const uint8_t *msg, uint8_t owner)
{
    using namespace neotree;
    if (owner == 0)
    {
        return false;
    }
    switch (static_cast<serial_msg_type>(msg[0]))
    {
    case serial_msg_type::ENTITY_SPAWN:
        if (msg[3] > static_cast<uint8_t>(DirectKind::brush))
        {
            return false;
        }
        return direct_spawn(engine, msg[1], owner, msg[2], static_cast<DirectKind>(msg[3]), read_vec(msg + 4),
                            read_vec(msg + 10), to_rgb(msg[16], msg[17], msg[18]), static_cast<float>(msg[19]));
    case serial_msg_type::ENTITY_KILL:
        direct_kill(engine, owner, msg[1]);
        return true;
    case serial_msg_type::BRUSH:
        return apply_brush(msg, owner);
    default:
        return false;
    }
}

// Applies the stream's newest brush samples (taken under the lock, applied
// outside it).
static void apply_stream()
{
    if (stream_lock == nullptr)
    {
        return;
    }
    for (uint8_t o = 1; o < stream_owners; o++)
    {
        for (uint8_t b = 0; b < stream_brushes; b++)
        {
            uint8_t msg[brush_len];
            uint32_t irq = spin_lock_blocking(stream_lock);
            const bool fresh = stream_box[o][b].fresh;
            if (fresh)
            {
                memcpy(msg, stream_box[o][b].msg, sizeof(msg));
                stream_box[o][b].fresh = false;
            }
            spin_unlock(stream_lock, irq);
            if (fresh)
            {
                apply_brush(msg, o);
            }
        }
    }
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
        case 3: return lib.delete_effect(msg[2]);
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

uint32_t read_u32(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
}

// SCHEDULE_TIMER, SCHEDULE_EVENT, SCHEDULE_DELETE, SCHEDULE_RUN.
static bool apply_schedule_command(const uint8_t *msg)
{
    using namespace neotree;
    Director &d = engine.director();
    Library &lib = d.library();
    switch (static_cast<serial_msg_type>(msg[0]))
    {
    case serial_msg_type::SCHEDULE_TIMER:
    {
        TimerRule r;
        r.enabled = (msg[2] & 1) != 0;
        r.days = msg[3];
        r.on_s = read_u32(msg + 4);
        r.off_s = read_u32(msg + 8);
        return lib.set_timer(msg[1], r);
    }
    case serial_msg_type::SCHEDULE_EVENT:
    {
        static ScheduledEvent e;   // static: keep it off core0's stack
        e = ScheduledEvent{};
        e.enabled = (msg[2] & 1) != 0;
        e.repeat = static_cast<Repeat>(msg[3]);
        e.days = msg[4];
        e.year = static_cast<int16_t>(msg[5] | (msg[6] << 8));
        e.month = msg[7];
        e.day = msg[8];
        e.time_s = read_u32(msg + 9);
        e.action = static_cast<EventAction>(msg[13]);
        e.duration_s = read_u32(msg + 14);
        if (!copy_name(e.name, reinterpret_cast<const char *>(msg + 18), protocol_name_len))
        {
            return false;
        }
        memcpy(e.target, msg + 38, sizeof(e.target));
        e.target[sizeof(e.target) - 1] = '\0';
        return lib.set_event(msg[1], e);
    }
    case serial_msg_type::SCHEDULE_DELETE:
        return msg[1] == 0 ? lib.delete_timer(msg[2]) : msg[1] == 1 && lib.delete_event(msg[2]);
    case serial_msg_type::SCHEDULE_RUN:
        if (msg[1] == 0xFF)
        {
            d.end_event(engine);
            return true;
        }
        return d.start_event(engine, msg[1]);
    default:
        return false;
    }
}

// FX_EDIT, FX_SET, FX_ITEM, FX_GET, FX_SAVE: the draft effect.
static bool apply_effect_command(const uint8_t *msg, uint8_t owner)
{
    using namespace neotree;
    Director &d = engine.director();
    const auto section = static_cast<EffectSection>(msg[1]);
    switch (static_cast<serial_msg_type>(msg[0]))
    {
    case serial_msg_type::FX_EDIT:
        return d.edit_effect(engine, msg[1], msg[2]);
    case serial_msg_type::FX_SET:
    {
        FieldValue v;
        v.f = read_f32(msg + 4);
        v.c = to_rgb(msg[8], msg[9], msg[10]);
        return std::isfinite(v.f) && d.edit_field(engine, section, msg[2], msg[3], v);
    }
    case serial_msg_type::FX_ITEM:
        return d.edit_item(engine, msg[1], static_cast<EffectSection>(msg[2]), msg[3]);
    case serial_msg_type::FX_GET:
        if (msg[1] >= static_cast<uint8_t>(EffectSection::count) || owner == 0 || owner > engine_host_owner_usb)
        {
            return false;
        }
        fx_wanted[owner] = static_cast<uint8_t>(fx_wanted[owner] | (1u << msg[1]));
        return true;
    case serial_msg_type::FX_SAVE:
    {
        char name[name_size];
        return copy_name(name, reinterpret_cast<const char *>(msg + 1), protocol_name_len) &&
               d.save_draft(name) != no_index;
    }
    default:
        return false;
    }
}

static bool apply_mode_command(const uint8_t *msg, size_t len, uint8_t owner)
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
    case serial_msg_type::BENCH:
        run_bench();
        break;
    case serial_msg_type::ENTITY_SPAWN:
    case serial_msg_type::ENTITY_KILL:
    case serial_msg_type::BRUSH:
        // No scene change to publish: entities aren't part of it.
        return apply_direct_command(msg, owner);
    case serial_msg_type::FX_SET:
    case serial_msg_type::FX_GET:
        // Scene changes (if any) are published with the frame.
        return apply_effect_command(msg, owner);
    case serial_msg_type::FX_EDIT:
    case serial_msg_type::FX_ITEM:
    case serial_msg_type::FX_SAVE:
        ok = apply_effect_command(msg, owner);
        break;
    case serial_msg_type::SCHEDULE_TIMER:
    case serial_msg_type::SCHEDULE_EVENT:
    case serial_msg_type::SCHEDULE_DELETE:
    case serial_msg_type::SCHEDULE_RUN:
        ok = apply_schedule_command(msg);
        break;
    default:
        return false;
    }
    publish_scene();
    return ok;
}

static bool queue_command(const uint8_t *msg, size_t len, uint8_t owner = 0)
{
    if (len > sizeof(pending[0].bytes) || pending_count >= max_pending_commands)
    {
        return false;
    }
    memcpy(pending[pending_count].bytes, msg, len);
    pending[pending_count].len = static_cast<uint8_t>(len);
    pending[pending_count].owner = owner;
    pending_count++;
    return true;
}

void engine_host_stream_brush(uint8_t owner, const uint8_t *msg)
{
    if (stream_lock == nullptr || owner == 0 || owner >= stream_owners)
    {
        return;
    }
    stream_sample &s = stream_box[owner][msg[2] % stream_brushes];
    uint32_t irq = spin_lock_blocking(stream_lock);
    memcpy(s.msg, msg, sizeof(s.msg));
    s.fresh = true;
    spin_unlock(stream_lock, irq);
}

void engine_host_forget_owner(uint8_t owner)
{
    if (stream_lock == nullptr || owner == 0 || owner >= stream_owners)
    {
        return;
    }
    uint32_t irq = spin_lock_blocking(stream_lock);
    for (stream_sample &s : stream_box[owner])
    {
        s.fresh = false;
    }
    spin_unlock(stream_lock, irq);
}

bool engine_host_set_demo(uint8_t id)
{
    const uint8_t msg[2] = {static_cast<uint8_t>(serial_msg_type::DEMO), id};
    return queue_command(msg, sizeof(msg));
}

bool engine_host_mode_command(const uint8_t *msg, size_t len, uint8_t owner)
{
    return queue_command(msg, len, owner);
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

uint8_t engine_host_fx_reply_owner()
{
    return fx_reply_owner;
}

size_t engine_host_take_fx_reply(char *out, size_t cap)
{
    if (fx_reply_owner == 0 || cap == 0)
    {
        return 0;
    }
    __dmb();   // the flag before the reply it guards
    const size_t n = fx_reply_len < cap - 1 ? fx_reply_len : cap - 1;
    memcpy(out, fx_reply, n);
    out[n] = '\0';
    __dmb();
    fx_reply_owner = 0;
    return n;
}

void engine_host_set_output(bool enabled)
{
    engine.director().set_lights(engine, enabled);
}

size_t engine_host_schedule_json(char *out, size_t cap)
{
    return copy_snapshot(schedule_json, schedule_json_max, out, cap);
}

void engine_host_frame(uint64_t now_us, uint32_t *words, size_t count)
{
    for (uint8_t i = 0; i < pending_count; i++)
    {
        // A refused brush sample isn't worth an event: they come 30-60 a second.
        if (!apply_mode_command(pending[i].bytes, pending[i].len, pending[i].owner) &&
            pending[i].bytes[0] != static_cast<uint8_t>(serial_msg_type::BRUSH))
        {
            event_logf("mode command %u rejected", (unsigned)pending[i].bytes[0]);
        }
    }
    pending_count = 0;
    apply_stream();
    pump_fx_replies();

    // The schedule, once the clock is known; the lights as it leaves them.
    int64_t unix_us = 0;
    if (clock_unix_us(&unix_us))
    {
        static neotree::TimeZone zone;   // static: keep it off core0's stack
        clock_zone(&zone);
        engine.director().run_schedule(engine, unix_us / 1000, zone);
    }
    tree_output_enabled = engine.director().lights();

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
    stats.direct_entities = neotree::direct_count(engine);
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
    s.direct_entities = stats.direct_entities;
    return s;
}
