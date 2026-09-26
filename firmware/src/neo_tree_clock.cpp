#include "neo_tree_clock.hpp"

#include <cstdio>
#include <cstring>

#include "hardware/sync.h"
#include "lwip/apps/sntp.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"

#include "neo_tree_event_log.hpp"
#include "neo_tree_scene_store.hpp"

namespace {

// Nothing earlier is a real time: before the first sync the clock SNTP sees
// starts here (the round-trip compensation needs it within ~34 years of
// the truth), and times before it are refused.
constexpr int64_t clock_floor_unix_s = 1'780'000'000;   // 2026-05-28
// The app's time is only a fallback: SNTP's wins for this long.
constexpr uint64_t sntp_trusted_us = 2ull * 3600 * 1'000'000;

spin_lock_t *lock = nullptr;
// Under lock:
bool is_set = false;
clock_source source = clock_source::none;
int64_t offset_us = 0;           // Unix us = time_us_64() + offset_us
uint64_t last_sync_us = 0;       // time_us_64() at the last accepted time
uint64_t last_sntp_us = 0;
uint32_t sntp_syncs = 0;
uint32_t app_sets = 0;
uint32_t app_ignored = 0;
int32_t last_step_ms = 0;
char tz_text[64] = "";
bool tz_stored = false;
neotree::TimeZone zone{};

bool sntp_started = false;   // core1 only

uint32_t lock_irq() { return spin_lock_blocking(lock); }
void unlock_irq(uint32_t irq) { spin_unlock(lock, irq); }

// Takes a time (under the lock).
void accept(int64_t unix_us, clock_source from)
{
    const uint64_t now = time_us_64();
    const int64_t new_offset = unix_us - static_cast<int64_t>(now);
    last_step_ms = is_set ? static_cast<int32_t>((new_offset - offset_us) / 1000) : 0;
    offset_us = new_offset;
    is_set = true;
    source = from;
    last_sync_us = now;
}

// The settings text with the tz line replaced (other lines kept); tz null:
// without one.
void settings_with_tz(const char *tz, char *out, size_t cap)
{
    static char old[scene_store_settings_max + 1];
    scene_store_load_settings(old, sizeof(old));
    size_t n = 0;
    for (const char *line = old; *line != '\0';)
    {
        const char *end = strchr(line, '\n');
        const size_t len = end != nullptr ? static_cast<size_t>(end - line) : strlen(line);
        if (strncmp(line, "tz=", 3) != 0 && len > 0 && n + len + 1 < cap)
        {
            memcpy(out + n, line, len);
            n += len;
            out[n++] = '\n';
        }
        line += len + (end != nullptr ? 1 : 0);
    }
    out[n] = '\0';
    if (tz != nullptr)
    {
        snprintf(out + n, cap - n, "tz=%s\n", tz);
    }
}

}  // namespace

// lwIP's SNTP client (lwipopts.h), in its callbacks on core1.
extern "C" void neo_tree_clock_sntp_set(uint32_t sec, uint32_t us)
{
    if (static_cast<int64_t>(sec) < clock_floor_unix_s)
    {
        return;
    }
    // No logging here (lwIP IRQ context): clock_poll_core1 reports it.
    const uint32_t irq = lock_irq();
    accept(static_cast<int64_t>(sec) * 1'000'000 + us, clock_source::sntp);
    last_sntp_us = last_sync_us;
    sntp_syncs++;
    unlock_irq(irq);
}

extern "C" void neo_tree_clock_sntp_get(uint32_t *sec, uint32_t *us)
{
    int64_t now = 0;
    if (!clock_unix_us(&now))
    {
        now = clock_floor_unix_s * 1'000'000 + static_cast<int64_t>(time_us_64());
    }
    *sec = static_cast<uint32_t>(now / 1'000'000);
    *us = static_cast<uint32_t>(now % 1'000'000);
}

void clock_init()
{
    lock = spin_lock_init(spin_lock_claim_unused(true));
    static_assert(sizeof(clock_default_tz) <= sizeof(tz_text));
    memcpy(tz_text, clock_default_tz, sizeof(clock_default_tz));
    static char text[scene_store_settings_max + 1];
    scene_store_load_settings(text, sizeof(text));
    const char *tz = strstr(text, "tz=");
    if (tz != nullptr && (tz == text || tz[-1] == '\n'))
    {
        char rule[sizeof(tz_text)] = "";
        const size_t len = strcspn(tz + 3, "\n");
        if (len < sizeof(rule))
        {
            memcpy(rule, tz + 3, len);
            rule[len] = '\0';
            if (neotree::parse_time_zone(rule, zone))
            {
                memcpy(tz_text, rule, sizeof(rule));
                tz_stored = true;
                event_logf("clock: time zone %s", tz_text);
                return;
            }
        }
        event_logf("clock: stored time zone unreadable - the default");
    }
    neotree::parse_time_zone(tz_text, zone);
}

void clock_poll_core1()
{
    // SNTP syncs: the first, and any that moved the clock much.
    static uint32_t reported = 0;
    const uint32_t irq = lock_irq();
    const uint32_t count = sntp_syncs;
    const int32_t step = last_step_ms;
    unlock_irq(irq);
    if (count != reported)
    {
        reported = count;
        if (count == 1)
        {
            event_logf("clock: set by SNTP");
        }
        else if (step > 1000 || step < -1000)
        {
            event_logf("clock: SNTP moved it %ld ms", static_cast<long>(step));
        }
    }
    if (sntp_started || cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) != CYW43_LINK_UP)
    {
        return;
    }
    sntp_started = true;
    cyw43_arch_lwip_begin();
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_setservername(1, "time.google.com");
    sntp_init();
    cyw43_arch_lwip_end();
}

bool clock_unix_us(int64_t *out)
{
    const uint32_t irq = lock_irq();
    const bool ok = is_set;
    const int64_t offset = offset_us;
    unlock_irq(irq);
    *out = ok ? static_cast<int64_t>(time_us_64()) + offset : 0;
    return ok;
}

bool clock_local(neotree::CivilTime *out)
{
    const uint32_t irq = lock_irq();
    const bool ok = is_set;
    const int64_t unix_us = static_cast<int64_t>(time_us_64()) + offset_us;
    const neotree::TimeZone z = zone;   // a copy: core0 may replace it
    unlock_irq(irq);
    if (ok)
    {
        *out = neotree::local_time(unix_us / 1'000'000, z);
    }
    return ok;
}

void clock_zone(neotree::TimeZone *out)
{
    const uint32_t irq = lock_irq();
    *out = zone;
    unlock_irq(irq);
}

bool clock_set_from_app(int64_t unix_ms)
{
    if (unix_ms / 1000 < clock_floor_unix_s)
    {
        return false;
    }
    const uint32_t irq = lock_irq();
    const bool sntp_fresh = sntp_syncs > 0 && time_us_64() - last_sntp_us < sntp_trusted_us;
    if (sntp_fresh)
    {
        app_ignored++;
    }
    else
    {
        accept(unix_ms * 1000, clock_source::app);
        app_sets++;
    }
    unlock_irq(irq);
    return !sntp_fresh;
}

bool clock_set_zone(const char *tz, size_t len)
{
    static char text[scene_store_settings_max + 1];
    if (len == 0)
    {
        // Back to the default, and none stored.
        if (!tz_stored)
        {
            return true;
        }
        const uint32_t irq = lock_irq();
        memcpy(tz_text, clock_default_tz, sizeof(clock_default_tz));
        neotree::parse_time_zone(tz_text, zone);
        tz_stored = false;
        unlock_irq(irq);
        settings_with_tz(nullptr, text, sizeof(text));
        scene_store_save_settings(text);
        event_logf("clock: time zone back to the default");
        return true;
    }
    char rule[sizeof(tz_text)];
    if (len >= sizeof(rule))
    {
        return false;
    }
    memcpy(rule, tz, len);
    rule[len] = '\0';
    neotree::TimeZone z;
    if (!neotree::parse_time_zone(rule, z))
    {
        return false;
    }
    if (strcmp(rule, tz_text) == 0 && tz_stored)
    {
        return true;   // already so: no flash write
    }
    const uint32_t irq = lock_irq();
    zone = z;
    memcpy(tz_text, rule, sizeof(rule));
    tz_stored = true;
    unlock_irq(irq);
    settings_with_tz(rule, text, sizeof(text));
    const bool stored = scene_store_save_settings(text);
    event_logf("clock: time zone %s%s", rule, stored ? "" : " (not stored - flash write failed)");
    return true;
}

clock_status_t clock_status()
{
    clock_status_t s = {};
    const uint64_t now = time_us_64();
    const uint32_t irq = lock_irq();
    s.set = is_set;
    s.source = source;
    s.unix_ms = is_set ? (static_cast<int64_t>(now) + offset_us) / 1000 : 0;
    s.sync_age_s = is_set ? static_cast<uint32_t>((now - last_sync_us) / 1'000'000) : 0;
    s.sntp_syncs = sntp_syncs;
    s.sntp_age_s = sntp_syncs > 0 ? static_cast<uint32_t>((now - last_sntp_us) / 1'000'000) : UINT32_MAX;
    s.app_sets = app_sets;
    s.app_ignored = app_ignored;
    s.last_step_ms = last_step_ms;
    memcpy(s.tz, tz_text, sizeof(s.tz));
    s.tz_stored = tz_stored;
    unlock_irq(irq);
    return s;
}
