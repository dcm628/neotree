#include "neo_tree_wifi.hpp"

#include <stdio.h>
#include <cstring>

#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"  // XIP_BASE
#include "pico/cyw43_arch.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/dhcp.h"
#include "lwip/prot/dhcp.h"
#include "lwip/dns.h"

#include "neo_tree_config.hpp"
#include "neo_tree_event_log.hpp"

// ---- Persisted credentials ----

// "WIFI" little-endian. Erased flash reads 0xFFFFFFFF, so a sector that was
// never written (or was written by a build predating this) is rejected.
static const uint32_t wifi_record_magic = 0x49464957;

struct wifi_credentials_record
{
    uint32_t magic;
    uint8_t ssid_len;
    uint8_t password_len;
    char ssid[wifi_ssid_max_len + 1];
    char password[wifi_password_max_len + 1];
}__packed;
static_assert(sizeof(wifi_credentials_record) <= FLASH_SECTOR_SIZE, "wifi record must fit one sector");

// One sector directly below the LED position config region.
static uint32_t wifi_flash_offset()
{
    return pos_config_flash_offset() - FLASH_SECTOR_SIZE;
}

static const wifi_credentials_record *flash_record()
{
    return reinterpret_cast<const wifi_credentials_record *>(XIP_BASE + wifi_flash_offset());
}

static bool record_valid(const wifi_credentials_record *r)
{
    return r->magic == wifi_record_magic && r->ssid_len >= 1 && r->ssid_len <= wifi_ssid_max_len &&
           r->password_len <= wifi_password_max_len && r->ssid[r->ssid_len] == '\0' &&
           r->password[r->password_len] == '\0';
}

// ---- core1: connection management ----

// How long a connect attempt gets to reach CYW43_LINK_UP before it's
// abandoned and retried (join + WPA handshake + DHCP normally take 2-5s).
static const uint64_t connect_timeout_us = 30'000'000;
// A join (association + WPA handshake) resolves either way in ~3s. One
// still "joining" after this is stuck - seen once in 8 boots, silently
// hanging until the 30s timeout above - so it's retried sooner.
static const uint64_t join_timeout_us = 8'000'000;
// lwIP sends its first DHCP DISCOVER the instant the link comes up, and in 6
// of 8 boots it went unanswered (likely sent just before the AP forwards
// traffic), costing lwIP's ~2s retry timer. If no OFFER has arrived this
// long after joining, send one fresh DISCOVER straight away.
static const uint64_t dhcp_nudge_us = 500'000;
// Gap before the next connect attempt after a failure or timeout: short for
// the first few, then backing off so a genuinely wrong password doesn't hammer
// the router. The first join after every reboot fails (measured 2026-09-24:
// 8/8 boots, WPA handshake failure ~2.9s in, retry succeeds) - most likely
// the router still holding the session the Pico never closed before it
// reset. A fixed 10s wait here made every boot take ~17-18s to get online.
static const uint32_t quick_retries = 3;
static const uint64_t quick_retry_us = 1'000'000;
static const uint64_t slow_retry_us = 10'000'000;

// core1's working copy, loaded from flash at start and after each commit.
static wifi_credentials_record active = {};
static bool configured = false;

static bool connect_in_progress = false;
static uint64_t connect_started_us = 0;
// Earliest time the next connect attempt may start - set when an attempt
// fails or times out, so the printed "retrying in" matches reality.
static uint64_t retry_not_before_us = 0;
static uint64_t link_up_since_us = 0;
static uint64_t joined_us = 0;       // when the link last reached NOIP (joined, no IP yet)
static bool dhcp_nudged = false;
static int last_link_status = CYW43_LINK_DOWN;
static uint32_t attempts_since_up = 0;
static uint32_t connect_count = 0;

static uint64_t retry_delay_us()
{
    return attempts_since_up <= quick_retries ? quick_retry_us : slow_retry_us;
}

// Set by core0 after a commit has been written to flash.
static volatile bool credentials_changed = false;

static const char *link_status_name(int status)
{
    switch (status)
    {
    case CYW43_LINK_DOWN: return "down";
    case CYW43_LINK_JOIN: return "joining";
    case CYW43_LINK_NOIP: return "joined, waiting for DHCP";
    case CYW43_LINK_UP: return "up";
    case CYW43_LINK_FAIL: return "failed";
    case CYW43_LINK_NONET: return "network not found";
    // The driver reports any WPA handshake failure this way, not only a
    // wrong password.
    case CYW43_LINK_BADAUTH: return "authentication failed";
    default: return "unknown";
    }
}

// ip4addr_ntoa() returns a static buffer; copy out under the lwIP lock since
// the netif is updated from the CYW43 background IRQ.
static const char *sta_ip_str()
{
    static char ip[16];
    cyw43_arch_lwip_begin();
    snprintf(ip, sizeof(ip), "%s", ip4addr_ntoa(netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA])));
    cyw43_arch_lwip_end();
    return ip;
}

static void print_connected_report(uint32_t connect_ms)
{
    char ip[16], netmask[16], gateway[16], dns[16], hostname[33];
    cyw43_arch_lwip_begin();
    const struct netif *n = &cyw43_state.netif[CYW43_ITF_STA];
    snprintf(ip, sizeof(ip), "%s", ip4addr_ntoa(netif_ip4_addr(n)));
    snprintf(netmask, sizeof(netmask), "%s", ip4addr_ntoa(netif_ip4_netmask(n)));
    snprintf(gateway, sizeof(gateway), "%s", ip4addr_ntoa(netif_ip4_gw(n)));
    snprintf(dns, sizeof(dns), "%s", ipaddr_ntoa(dns_getserver(0)));
    snprintf(hostname, sizeof(hostname), "%s", netif_get_hostname(n) ? netif_get_hostname(n) : "?");
    cyw43_arch_lwip_end();

    int32_t rssi = 0;
    cyw43_wifi_get_rssi(&cyw43_state, &rssi);
    uint8_t mac[6] = {}, bssid[6] = {};
    cyw43_wifi_get_mac(&cyw43_state, CYW43_ITF_STA, mac);
    cyw43_wifi_get_bssid(&cyw43_state, bssid);
    // WLC_GET_CHANNEL fills a channel_info_t {hw, target, scan}; hw is the
    // channel actually in use.
    uint32_t channel_info[3] = {};
    cyw43_ioctl(&cyw43_state, CYW43_IOCTL_GET_CHANNEL, sizeof(channel_info), (uint8_t *)channel_info,
                CYW43_ITF_STA);

    printf("wifi: connected to '%s' in %ums (attempt %u)\n", active.ssid, (unsigned)connect_ms,
           (unsigned)attempts_since_up);
    printf("wifi:   ip %s  netmask %s  gateway %s  dns %s\n", ip, netmask, gateway, dns);
    printf("wifi:   hostname %s  mac %02x:%02x:%02x:%02x:%02x:%02x  ap %02x:%02x:%02x:%02x:%02x:%02x  "
           "channel %u  rssi %d dBm\n",
           hostname, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], bssid[0], bssid[1], bssid[2], bssid[3],
           bssid[4], bssid[5], (unsigned)channel_info[0], (int)rssi);
}

static void load_credentials()
{
    configured = record_valid(flash_record());
    if (configured)
    {
        active = *flash_record();
    }
    else
    {
        memset(&active, 0, sizeof(active));
    }
}

// ---- connection trace (diagnostic) ----
// The first ~24 connection events since boot, timestamped, printed in every
// heartbeat - a one-off printf at boot is lost while USB re-enumerates.
//   C<n>      connect attempt n started
//   L<s>      link status changed to s (1 joining, 2 joined/no IP, 3 up,
//             negative = failure)
//   N1        re-sent the DHCP DISCOVER (no OFFER 0.5s after joining)
//   D<s>/<t>  lwIP DHCP state s after t tries (6 selecting = DISCOVER sent,
//             waiting for OFFER; 1 requesting = REQUEST sent, waiting for
//             ACK; 10 bound)
struct trace_entry
{
    uint32_t t_ms;
    char kind;
    int8_t a;
    uint8_t b;
};
static const uint32_t trace_max = 24;
static trace_entry trace[trace_max];
static uint32_t trace_len = 0;
static int last_dhcp_state = -1;
static int last_dhcp_tries = -1;

static void trace_add(char kind, int a, int b = 0)
{
    if (trace_len < trace_max)
    {
        trace[trace_len++] = {(uint32_t)(time_us_64() / 1000), kind, (int8_t)a, (uint8_t)b};
    }
}

static void trace_dhcp()
{
    const struct dhcp *d = netif_dhcp_data(&cyw43_state.netif[CYW43_ITF_STA]);
    if (d != nullptr && (d->state != last_dhcp_state || d->tries != last_dhcp_tries))
    {
        last_dhcp_state = d->state;
        last_dhcp_tries = d->tries;
        trace_add('D', d->state, d->tries);
    }
}

void wifi_format_trace(char *out, size_t cap)
{
    int n = 0;
    out[0] = '\0';
    for (uint32_t i = 0; i < trace_len && n < (int)cap - 16; i++)
    {
        const trace_entry &e = trace[i];
        if (e.kind == 'D')
        {
            n += snprintf(out + n, cap - n, "%s%u:D%d/%u", i ? " " : "", (unsigned)e.t_ms, e.a, e.b);
        }
        else
        {
            n += snprintf(out + n, cap - n, "%s%u:%c%d", i ? " " : "", (unsigned)e.t_ms, e.kind, e.a);
        }
    }
}

void wifi_print_trace()
{
    char line[400];
    wifi_format_trace(line, sizeof(line));
    printf("wifi trace (ms since boot): %s\n", line);
}

// ---- cached metrics for the status snapshot ----
// Refreshed from wifi_poll() once a second, so the status builder (which runs
// in lwIP IRQ context for network requests) never has to talk to the WiFi
// chip itself.
static wifi_snapshot_t snapshot = {};
static uint64_t last_snapshot_us = 0;
static const uint64_t snapshot_interval_us = 1'000'000;

static void refresh_snapshot(uint64_t now_us, int status)
{
    wifi_snapshot_t s = {};
    s.configured = configured;
    s.link_status = status;
    s.link_name = link_status_name(status);
    snprintf(s.ssid, sizeof(s.ssid), "%s", active.ssid);
    s.connects = connect_count;
    s.attempts = attempts_since_up;
    cyw43_wifi_get_mac(&cyw43_state, CYW43_ITF_STA, s.mac);
    if (status == CYW43_LINK_UP)
    {
        s.up_for_s = (uint32_t)((now_us - link_up_since_us) / 1'000'000);
        cyw43_wifi_get_rssi(&cyw43_state, &s.rssi);
        cyw43_wifi_get_bssid(&cyw43_state, s.bssid);
        uint32_t channel_info[3] = {};
        cyw43_ioctl(&cyw43_state, CYW43_IOCTL_GET_CHANNEL, sizeof(channel_info), (uint8_t *)channel_info,
                    CYW43_ITF_STA);
        s.channel = channel_info[0];
        cyw43_arch_lwip_begin();
        const struct netif *n = &cyw43_state.netif[CYW43_ITF_STA];
        snprintf(s.ip, sizeof(s.ip), "%s", ip4addr_ntoa(netif_ip4_addr(n)));
        snprintf(s.gateway, sizeof(s.gateway), "%s", ip4addr_ntoa(netif_ip4_gw(n)));
        snprintf(s.dns, sizeof(s.dns), "%s", ipaddr_ntoa(dns_getserver(0)));
        cyw43_arch_lwip_end();
    }
    snapshot = s;
    last_snapshot_us = now_us;
}

wifi_snapshot_t wifi_snapshot()
{
    return snapshot;
}

static volatile bool reconnect_requested = false;

void wifi_request_reconnect()
{
    reconnect_requested = true;
}

static void begin_connect()
{
    connect_started_us = time_us_64();
    attempts_since_up++;
    trace_add('C', attempts_since_up);
    bool open_network = (active.password_len == 0);
    event_logf("wifi connecting to '%s' (%s, attempt %u)", active.ssid, open_network ? "open" : "WPA2",
               (unsigned)attempts_since_up);
    int err = cyw43_arch_wifi_connect_async(active.ssid, open_network ? nullptr : active.password,
                                            open_network ? CYW43_AUTH_OPEN : CYW43_AUTH_WPA2_AES_PSK);
    connect_in_progress = (err == 0);
    if (err != 0)
    {
        event_logf("wifi connect failed to start (err=%d), retrying in %us", err,
                   (unsigned)(retry_delay_us() / 1000000));
        retry_not_before_us = connect_started_us + retry_delay_us();
    }
}

void wifi_start()
{
    cyw43_arch_enable_sta_mode();
    // The default power-save mode sleeps the radio between AP beacons, which
    // measured 5-240ms (avg ~90ms) ping latency. This is a mains-powered
    // control link, so trade the power for responsiveness.
    // (enable_sta_mode resets PM to the default, so this must come after.)
    cyw43_wifi_pm(&cyw43_state, CYW43_NONE_PM);
    load_credentials();
    if (!configured)
    {
        event_logf("wifi: no credentials stored (provision with tools/set_wifi.py)");
        return;
    }
    begin_connect();
}

void wifi_poll()
{
    if (credentials_changed)
    {
        credentials_changed = false;
        cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
        load_credentials();
        connect_in_progress = false;
        last_link_status = CYW43_LINK_DOWN;
        attempts_since_up = 0;
        event_logf("wifi credentials changed%s", configured ? "" : ", now offline");
        if (configured)
        {
            begin_connect();
        }
    }
    if (reconnect_requested && configured)
    {
        reconnect_requested = false;
        event_logf("wifi reconnect requested - leaving '%s'", active.ssid);
        cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
        connect_in_progress = false;
        last_link_status = CYW43_LINK_DOWN;
        attempts_since_up = 0;
        begin_connect();
    }
    uint64_t now_us = time_us_64();
    if (!configured)
    {
        if (now_us - last_snapshot_us >= snapshot_interval_us)
        {
            refresh_snapshot(now_us, CYW43_LINK_DOWN);
        }
        return;
    }
    int status = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
    if (status != last_link_status || now_us - last_snapshot_us >= snapshot_interval_us)
    {
        refresh_snapshot(now_us, status);
    }
    if (status != last_link_status)
    {
        trace_add('L', status);
        if (status == CYW43_LINK_NOIP)
        {
            joined_us = now_us;
            dhcp_nudged = false;
        }
    }
    trace_dhcp();
    if (status == CYW43_LINK_NOIP && !dhcp_nudged && now_us - joined_us >= dhcp_nudge_us)
    {
        dhcp_nudged = true;
        struct netif *sta = &cyw43_state.netif[CYW43_ITF_STA];
        cyw43_arch_lwip_begin();
        const struct dhcp *d = netif_dhcp_data(sta);
        if (d != nullptr && d->state == DHCP_STATE_SELECTING)
        {
            dhcp_network_changed_link_up(sta);   // fresh DISCOVER, same transaction
            trace_add('N', 1);
        }
        cyw43_arch_lwip_end();
    }

    if (status == CYW43_LINK_UP && last_link_status != CYW43_LINK_UP)
    {
        connect_count++;
        link_up_since_us = now_us;
        connect_in_progress = false;
        // Re-assert power-save off on every join, not just at start - latency
        // was seen back at power-save levels after the router dropped us.
        cyw43_wifi_pm(&cyw43_state, CYW43_NONE_PM);
        uint32_t connect_ms = (uint32_t)((now_us - connect_started_us) / 1000);
        print_connected_report(connect_ms);
        refresh_snapshot(now_us, status);
        // Fits event_text_max: the SSID is in the status snapshot anyway.
        event_logf("wifi up: %s ch %u rssi %d in %ums (attempt %u)", snapshot.ip, (unsigned)snapshot.channel,
                   (int)snapshot.rssi, (unsigned)connect_ms, (unsigned)attempts_since_up);
        attempts_since_up = 0;
    }
    else if (status != CYW43_LINK_UP && last_link_status == CYW43_LINK_UP)
    {
        event_logf("wifi lost after %us up (now: %s), reconnecting",
                   (unsigned)((now_us - link_up_since_us) / 1000000), link_status_name(status));
        connect_in_progress = false;
    }
    else if (connect_in_progress && status < 0)
    {
        // FAIL/NONET/BADAUTH are terminal for this attempt.
        event_logf("wifi connect failed after %ums: %s, retrying in %us",
                   (unsigned)((now_us - connect_started_us) / 1000), link_status_name(status),
                   (unsigned)(retry_delay_us() / 1000000));
        connect_in_progress = false;
        retry_not_before_us = now_us + retry_delay_us();
    }
    else if (connect_in_progress && status == CYW43_LINK_JOIN && (now_us - connect_started_us) > join_timeout_us)
    {
        event_logf("wifi join stuck after %us, retrying in %us", (unsigned)(join_timeout_us / 1000000),
                   (unsigned)(retry_delay_us() / 1000000));
        connect_in_progress = false;
        retry_not_before_us = now_us + retry_delay_us();
    }
    else if (connect_in_progress && (now_us - connect_started_us) > connect_timeout_us)
    {
        event_logf("wifi connect timed out after %us (stuck at: %s), retrying in %us",
                   (unsigned)(connect_timeout_us / 1000000), link_status_name(status),
                   (unsigned)(retry_delay_us() / 1000000));
        connect_in_progress = false;
        retry_not_before_us = now_us + retry_delay_us();
    }
    last_link_status = status;

    if (status != CYW43_LINK_UP && !connect_in_progress && now_us >= retry_not_before_us)
    {
        begin_connect();
    }
}

const char *wifi_status_str()
{
    static char buf[128];
    if (!configured)
    {
        return "no credentials";
    }
    if (last_link_status == CYW43_LINK_UP)
    {
        int32_t rssi = 0;
        cyw43_wifi_get_rssi(&cyw43_state, &rssi);
        // Low nibble of the packed PM value is the mode: 0 = power-save off.
        uint32_t pm = 0xFFFFFFFF;
        cyw43_wifi_get_pm(&cyw43_state, &pm);
        snprintf(buf, sizeof(buf), "up %s ssid=%s rssi=%d pm=0x%x connects=%u", sta_ip_str(), active.ssid,
                 (int)rssi, (unsigned)pm, (unsigned)connect_count);
    }
    else
    {
        snprintf(buf, sizeof(buf), "%s ssid=%s connects=%u", link_status_name(last_link_status), active.ssid,
                 (unsigned)connect_count);
    }
    return buf;
}

// ---- core0: provisioning over serial ----

static wifi_credentials_record staged = {};
static uint8_t staged_received[2] = {0, 0};  // bytes received per wifi_cred_field

static void clear_staged()
{
    memset(&staged, 0, sizeof(staged));
    staged_received[0] = staged_received[1] = 0;
}

void wifi_handle_cred_chunk(const wifi_cred_chunk_t *chunk)
{
    if (chunk->field == wifi_cred_field::ssid && chunk->offset == 0)
    {
        clear_staged();
    }
    uint8_t field = static_cast<uint8_t>(chunk->field);
    char *dest = nullptr;
    size_t max_len = 0;
    if (chunk->field == wifi_cred_field::ssid)
    {
        dest = staged.ssid;
        max_len = wifi_ssid_max_len;
    }
    else if (chunk->field == wifi_cred_field::password)
    {
        dest = staged.password;
        max_len = wifi_password_max_len;
    }
    // Chunks must arrive in order (the sender waits for each ack), so a gap
    // means one was lost.
    if (dest == nullptr || chunk->len > wifi_cred_chunk_max_data || chunk->offset != staged_received[field] ||
        chunk->offset + chunk->len > max_len)
    {
        printf("WIFI_CRED error: bad chunk field=%u offset=%u len=%u (expected offset %u)\n", field,
               chunk->offset, chunk->len, field < 2 ? staged_received[field] : 0);
        clear_staged();
        return;
    }
    memcpy(dest + chunk->offset, chunk->data, chunk->len);
    staged_received[field] += chunk->len;
    printf("WIFI_CRED ack field=%u offset=%u len=%u\n", field, chunk->offset, chunk->len);
}

void wifi_handle_cred_commit(const wifi_cred_commit_t *commit)
{
    // static: a sector-sized buffer would eat most of this core's 4KB stack.
    static uint8_t sector[FLASH_SECTOR_SIZE];
    memset(sector, 0xFF, sizeof(sector));

    if (commit->ssid_len == 0)
    {
        // Leave the sector erased - record_valid() rejects it.
        flash_write_sectors_locked(wifi_flash_offset(), sector, sizeof(sector));
        clear_staged();
        credentials_changed = true;
        printf("WIFI_CRED cleared\n");
        event_logf("wifi credentials cleared over USB");
        return;
    }
    if (commit->ssid_len != staged_received[0] || commit->password_len != staged_received[1])
    {
        printf("WIFI_CRED error: commit lengths ssid=%u password=%u but received ssid=%u password=%u\n",
               commit->ssid_len, commit->password_len, staged_received[0], staged_received[1]);
        clear_staged();
        return;
    }
    staged.magic = wifi_record_magic;
    staged.ssid_len = commit->ssid_len;
    staged.password_len = commit->password_len;
    if (!record_valid(&staged))
    {
        printf("WIFI_CRED error: staged record failed validation\n");
        clear_staged();
        return;
    }
    memcpy(sector, &staged, sizeof(staged));
    flash_write_sectors_locked(wifi_flash_offset(), sector, sizeof(sector));
    clear_staged();

    bool verified = record_valid(flash_record()) && flash_record()->ssid_len == commit->ssid_len;
    if (!verified)
    {
        printf("WIFI_CRED error: flash verify failed\n");
        return;
    }
    credentials_changed = true;
    printf("WIFI_CRED saved ssid_len=%u password_len=%u\n", commit->ssid_len, commit->password_len);
    event_logf("wifi credentials saved over USB");
}
