#include "neo_tree_status.hpp"

#include <stdarg.h>
#include <stdio.h>

#include "hardware/sync.h"
#include "lwip/stats.h"

#include "neo_tree_command_queue.hpp"
#include "neo_tree_event_log.hpp"
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

    j.raw(",\"queue\":{\"level\":%u,\"dropped\":%u},\"core1_loops\":%u", (unsigned)command_queue_level(),
          (unsigned)command_queue_dropped(), (unsigned)core1_loop_counter);

    net_server_diag_t nd = net_server_diag();
    j.raw(",\"net\":{\"clients\":%u,\"commands\":%u,\"accepts\":%u,\"accept_errors\":%u,\"writes_deferred\":%u,"
          "\"write_failures\":%u,\"overflow_closes\":%u,\"status_dropped\":%u}",
          (unsigned)net_server_client_count(), (unsigned)net_server_commands_received(), (unsigned)nd.accepts,
          (unsigned)nd.accept_errors, (unsigned)nd.writes_deferred, (unsigned)nd.write_failures,
          (unsigned)nd.overflow_closes, (unsigned)nd.status_dropped);

    j.raw(",\"lwip\":{\"heap_used\":%u,\"heap_max\":%u,\"heap_size\":%u,\"heap_errors\":%u}",
          (unsigned)lwip_stats.mem.used, (unsigned)lwip_stats.mem.max, (unsigned)lwip_stats.mem.avail,
          (unsigned)lwip_stats.mem.err);

    wifi_snapshot_t w = wifi_snapshot();
    char bssid[18], mac[18], trace[400];
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
