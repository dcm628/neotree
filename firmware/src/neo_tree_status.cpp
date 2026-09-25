#include "neo_tree_status.hpp"

#include <stdarg.h>
#include <stdio.h>

#include "hardware/sync.h"
#include "lwip/stats.h"

#include "neo_tree_command_queue.hpp"
#include "neo_tree_event_log.hpp"
#include "neo_tree_engine.hpp"
#include "neo_tree_loop_monitor.hpp"
#include "neo_tree_safety.hpp"
#include "neo_tree_scene_store.hpp"
#include "neo_tree_led_output.hpp"
#include "neo_tree_net_server.hpp"
#include "neo_tree_protocol.hpp"
#include "neo_tree_wifi.hpp"

extern volatile uint32_t target_loop_rate;     // main.cpp
extern volatile uint32_t core1_loop_counter;   // main.cpp

bool status_boot_was_watchdog = false;

#if defined(RASPBERRYPI_PICO2_W)
static const char *board_name = "pico2_w";
#elif defined(RASPBERRYPI_PICO_W)
static const char *board_name = "pico_w";
#else
static const char *board_name = "unknown";
#endif

namespace
{
// Minimal append-only JSON writer over a fixed buffer. Once something doesn't
// fit, `full` latches and every later write is ignored - callers reserve room
// for the closing brackets via `room()` checks.
struct json_writer
{
    char *buf;
    size_t cap;
    size_t len = 0;
    bool full = false;

    void raw(const char *fmt, ...) __attribute__((format(printf, 2, 3)))
    {
        if (full)
        {
            return;
        }
        va_list args;
        va_start(args, fmt);
        int n = vsnprintf(buf + len, cap - len, fmt, args);
        va_end(args);
        if (n < 0 || (size_t)n >= cap - len)
        {
            full = true;
            buf[len] = '\0';   // drop the partial write
            return;
        }
        len += (size_t)n;
    }

    // A JSON string literal with quotes, backslashes and control characters escaped.
    void str(const char *s)
    {
        raw("\"");
        for (; *s && !full; s++)
        {
            unsigned char ch = (unsigned char)*s;
            if (ch == '"' || ch == '\\')
            {
                raw("\\%c", ch);
            }
            else if (ch < 0x20)
            {
                raw("\\u%04x", ch);
            }
            else
            {
                raw("%c", ch);
            }
        }
        raw("\"");
    }

    size_t room() const { return cap - len; }
};

void mac_str(char *out, const uint8_t *m)
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}
}  // namespace

static spin_lock_t *status_lock = nullptr;

void status_init()
{
    status_lock = spin_lock_init(spin_lock_claim_unused(true));
}

static size_t build_locked(char *out, size_t cap);

size_t status_build_json(char *out, size_t cap)
{
    // Serialised: a network request (core1, lwIP IRQ) and a serial one (core0)
    // could otherwise overlap on the shared event buffer below. Building takes
    // a few hundred microseconds.
    uint32_t irq = spin_lock_blocking(status_lock);
    size_t len = build_locked(out, cap);
    spin_unlock(status_lock, irq);
    return len;
}

static size_t build_locked(char *out, size_t cap)
{
    json_writer j{out, cap};
    uint64_t now_ms = time_us_64() / 1000;

    j.raw("{\"fw\":{\"build\":");
    j.str(__DATE__ " " __TIME__);
    j.raw(",\"board\":\"%s\"},\"uptime_ms\":%llu,\"reset\":\"%s\",\"lights_on\":%s", board_name,
          (unsigned long long)now_ms, status_boot_was_watchdog ? "watchdog" : "power-on",
          tree_output_enabled ? "true" : "false");

    led_output_stats_t led = led_output_peek_stats();
    led_output_tuning_t tuning = led_output_get_tuning();
    j.raw(",\"led\":{\"target_fps\":%u,\"frames\":%u,\"prepare_us\":%u,\"output_us\":%u,\"max_output_us\":%u,"
          "\"mode\":%u,\"stagger_ns\":%u,\"phases\":[%u,%u,%u,%u],\"underflows\":[%u,%u,%u,%u],"
          "\"overflows\":[%u,%u,%u,%u]}",
          (unsigned)target_loop_rate, (unsigned)led.frames, (unsigned)led.last_prepare_us,
          (unsigned)led.last_output_us, (unsigned)led.max_output_us, (unsigned)led_output_get_mode(),
          (unsigned)tuning.stagger_ns, tuning.phase_map & 3u, (tuning.phase_map >> 2) & 3u,
          (tuning.phase_map >> 4) & 3u, (tuning.phase_map >> 6) & 3u, (unsigned)led.underflows_total[0],
          (unsigned)led.underflows_total[1], (unsigned)led.underflows_total[2], (unsigned)led.underflows_total[3],
          (unsigned)led.overflows_total[0], (unsigned)led.overflows_total[1], (unsigned)led.overflows_total[2],
          (unsigned)led.overflows_total[3]);

    static engine_host_stats_t eng;   // static (as below): this runs in lwIP's IRQ on core1
    eng = engine_host_stats();
    j.raw(",\"engine\":{\"ticks\":%llu,\"ticks_dropped\":%llu,\"frames\":%llu,\"advance_us\":%u,"
          "\"max_advance_us\":%u,\"render_us\":%u,\"max_render_us\":%u,\"max_advance_at_ms\":%u,\"max_render_at_ms\":%u,"
          "\"slow_frames\":%u,\"led_evals\":%u,\"geometry_us\":%u,\"leds\":%u,\"positioned\":%u,"
          "\"rejected_edits\":%u,\"entities\":%u,\"direct_entities\":%u,\"spawns_failed\":%u,\"demo\":%u,\"rule_fires\":%u,"
          "\"events_dropped\":%u,\"actions_dropped\":%u,\"spawns_over_quota\":%u,\"peak_entities\":%u}",
          (unsigned long long)eng.ticks, (unsigned long long)eng.ticks_dropped, (unsigned long long)eng.frames,
          (unsigned)eng.last_advance_us, (unsigned)eng.max_advance_us, (unsigned)eng.last_render_us,
          (unsigned)eng.max_render_us, (unsigned)eng.max_advance_at_ms, (unsigned)eng.max_render_at_ms,
          (unsigned)eng.slow_frames, (unsigned)eng.led_evals, (unsigned)eng.geometry_build_us, (unsigned)eng.leds,
          (unsigned)eng.positioned, (unsigned)eng.rejected_edits, (unsigned)eng.entities, (unsigned)eng.direct_entities,
          (unsigned)eng.spawns_failed, (unsigned)eng.demo, (unsigned)eng.rule_fires, (unsigned)eng.events_dropped,
          (unsigned)eng.actions_dropped, (unsigned)eng.spawns_over_quota, (unsigned)eng.peak_entities);

    // Core0 main-loop stalls (neo_tree_loop_monitor): counts, then the most
    // recent as [uptime_ms, gap_us, pass flags].
    loop_monitor_stats_t lm = loop_monitor_stats();
    j.raw(",\"loop\":{\"passes\":%u,\"stalls_500us\":%u,\"stalls_2ms\":%u,\"stalls_10ms\":%u,"
          "\"max_gap_us\":%u,\"max_gap_at_ms\":%u,\"core0_irqs\":[\"0x%08x\",\"0x%08x\"],\"recent\":[",
          (unsigned)lm.passes, (unsigned)lm.stalls_500us, (unsigned)lm.stalls_2ms, (unsigned)lm.stalls_10ms,
          (unsigned)lm.max_gap_us, (unsigned)lm.max_gap_at_ms, (unsigned)lm.core0_irqs[0],
          (unsigned)lm.core0_irqs[1]);
    static loop_stall_t stalls[12];   // static: keep it off the stack
    size_t stall_count = loop_monitor_recent(stalls, 12);
    for (size_t i = 0; i < stall_count; i++)
    {
        j.raw("%s[%u,%u,%u]", i ? "," : "", (unsigned)stalls[i].at_ms, (unsigned)stalls[i].gap_us,
              (unsigned)stalls[i].flags);
    }
    // Core0 interrupt handlers: [irq, calls, max_us, calls >= 0.5 ms, total_us].
    j.raw("],\"irqs\":[");
    static loop_irq_stats_t irqs[loop_irq_slots];
    size_t irq_count = loop_monitor_irq_stats(irqs, loop_irq_slots);
    for (size_t i = 0; i < irq_count; i++)
    {
        j.raw("%s[%d,%u,%u,%u,%llu]", i ? "," : "", (int)irqs[i].irq, (unsigned)irqs[i].calls,
              (unsigned)irqs[i].max_us, (unsigned)irqs[i].slow, (unsigned long long)irqs[i].total_us);
    }
    j.raw("]}");

    safety_report_t sr = safety_report();
    j.raw(",\"safety\":{\"crash_reboot\":%s,\"crash_count\":%u,\"fault_core\":%d,\"fault_pc\":\"0x%08lx\","
          "\"fault_lr\":\"0x%08lx\",\"stack_used\":[%u,%u],\"stack_size\":[%u,%u]}",
          sr.crash_reboot ? "true" : "false", (unsigned)sr.crash_count, (int)sr.fault_core,
          (unsigned long)sr.fault_pc, (unsigned long)sr.fault_lr, (unsigned)safety_stack_used(0),
          (unsigned)safety_stack_used(1), (unsigned)safety_stack_size(0), (unsigned)safety_stack_size(1));

    static char scene[2048];   // static: keep it off the stack
    engine_host_scene_json(scene, sizeof(scene));
    j.raw(",\"scene\":%s", scene[0] ? scene : "{}");

    // The stored library (neo_tree_scene_store).
    const scene_store_stats_t ss = scene_store_stats();
    static const char *const load_names[] = {"none", "loaded", "invalid"};
    j.raw(",\"library\":{\"load\":\"%s\",\"stored_bytes\":%u,\"effects_load\":\"%s\",\"effects_bytes\":%u,"
          "\"saves\":%u,\"save_failures\":%u,\"last_save_ms\":%u}",
          load_names[static_cast<int>(ss.load)], (unsigned)ss.stored_bytes,
          load_names[static_cast<int>(ss.effects_load)], (unsigned)ss.effects_bytes, (unsigned)ss.saves,
          (unsigned)ss.save_failures, (unsigned)(ss.last_save_us / 1000));

    j.raw(",\"queue\":{\"level\":%u,\"dropped\":%u},\"core1_loops\":%u", (unsigned)command_queue_level(),
          (unsigned)command_queue_dropped(), (unsigned)core1_loop_counter);

    net_server_diag_t nd = net_server_diag();
    j.raw(",\"net\":{\"clients\":%u,\"commands\":%u,\"accepts\":%u,\"accept_errors\":%u,\"writes_deferred\":%u,"
          "\"write_failures\":%u,\"overflow_closes\":%u,\"status_dropped\":%u,\"stream_rx\":%u,\"stream_old\":%u,"
          "\"stream_bad\":%u}",
          (unsigned)net_server_client_count(), (unsigned)net_server_commands_received(), (unsigned)nd.accepts,
          (unsigned)nd.accept_errors, (unsigned)nd.writes_deferred, (unsigned)nd.write_failures,
          (unsigned)nd.overflow_closes, (unsigned)nd.status_dropped, (unsigned)nd.stream_rx, (unsigned)nd.stream_old,
          (unsigned)nd.stream_bad);

    j.raw(",\"lwip\":{\"heap_used\":%u,\"heap_max\":%u,\"heap_size\":%u,\"heap_errors\":%u}",
          (unsigned)lwip_stats.mem.used, (unsigned)lwip_stats.mem.max, (unsigned)lwip_stats.mem.avail,
          (unsigned)lwip_stats.mem.err);

    static wifi_snapshot_t w;
    static char bssid[18], mac[18], trace[400];
    w = wifi_snapshot();
    mac_str(bssid, w.bssid);
    mac_str(mac, w.mac);
    wifi_format_trace(trace, sizeof(trace));
    j.raw(",\"wifi\":{\"configured\":%s,\"link\":", w.configured ? "true" : "false");
    j.str(w.link_name ? w.link_name : "unknown");
    j.raw(",\"link_code\":%d,\"ssid\":", w.link_status);
    j.str(w.ssid);
    j.raw(",\"ip\":\"%s\",\"gateway\":\"%s\",\"dns\":\"%s\",\"rssi\":%d,\"channel\":%u,\"bssid\":\"%s\","
          "\"mac\":\"%s\",\"connects\":%u,\"attempts\":%u,\"up_for_s\":%u,\"trace\":",
          w.ip, w.gateway, w.dns, (int)w.rssi, (unsigned)w.channel, bssid, mac, (unsigned)w.connects,
          (unsigned)w.attempts, (unsigned)w.up_for_s);
    j.str(trace);
    j.raw("}");

    // Events last, newest first, only while there's room to close the JSON.
    static event_entry events[event_log_capacity];   // static: ~1.8KB, off the stack
    size_t n = event_log_snapshot(events, event_log_capacity);
    j.raw(",\"events_total\":%u,\"events\":[", (unsigned)event_log_total());
    const size_t closing = 4;   // "]}" + NUL, with margin
    for (size_t i = 0; i < n; i++)
    {
        const event_entry &e = events[n - 1 - i];
        if (j.room() < closing + 16 + 2 * event_text_max)
        {
            break;
        }
        j.raw("%s[%u,", i ? "," : "", (unsigned)e.t_ms);
        j.str(e.text);
        j.raw("]");
    }
    j.full = false;   // the closing brackets were reserved above
    j.raw("]}");
    return j.len;
}
