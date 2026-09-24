#ifndef _NEO_TREE_WIFI_HPP
#define _NEO_TREE_WIFI_HPP

#include "pico/stdlib.h"

// WiFi station-mode connection management. Credentials are never compiled
// in: they're provisioned over USB serial (tools/set_wifi.py) and persisted
// in their own flash sector, just below the LED position config.

// ---- core1 (the core that ran cyw43_arch_init()) ----
// In threadsafe_background mode lwIP/CYW43 work runs in IRQs on that core.

// Loads stored credentials and starts a non-blocking connect. Stays offline
// if none are stored.
void wifi_start();

// Call every loop iteration. Tracks link state, re-issues the connect
// (rate-limited) whenever the link fails or drops, and picks up newly
// committed credentials.
void wifi_poll();

// One-line status for the heartbeat, e.g. "up 192.168.1.42 rssi=-51
// connects=1". Returned buffer is static - valid until the next call.
const char *wifi_status_str();

// Prints the boot connection trace (connect attempts, link and DHCP state
// changes, timestamped) - for diagnosing slow connects after power-up.
void wifi_print_trace();
// Same trace as one string (no prefix), for the status snapshot.
void wifi_format_trace(char *out, size_t cap);

// WiFi metrics cached by wifi_poll() once a second - safe to read from any
// core or IRQ context (a torn read is possible but harmless for diagnostics).
struct wifi_snapshot_t
{
    bool configured;
    int link_status;          // CYW43_LINK_*
    const char *link_name;
    char ssid[33];
    char ip[16];
    char gateway[16];
    char dns[16];
    int32_t rssi;             // dBm, when up
    uint32_t channel;
    uint8_t bssid[6];         // the access point
    uint8_t mac[6];           // this board
    uint32_t connects;        // successful connects since boot
    uint32_t attempts;        // attempts since the last successful connect
    uint32_t up_for_s;
};
wifi_snapshot_t wifi_snapshot();

// Any core: ask core1 to leave the network and rejoin (debug control).
void wifi_request_reconnect();

// ---- core0 (serial message handlers, called from process_msg()) ----

// Credentials arrive in pieces because every serial message has to fit in a
// single 64-byte USB packet (see max_group_update_entries in dcm_rgb.hpp).
const size_t wifi_ssid_max_len = 32;       // 802.11 limit
const size_t wifi_password_max_len = 64;   // WPA2 passphrase max (63) or 64-hex PSK
const size_t wifi_cred_chunk_max_data = 32;

enum class wifi_cred_field : uint8_t
{
    ssid = 0,
    password = 1,
};

struct wifi_cred_chunk_t
{
    wifi_cred_field field;
    uint8_t offset;
    uint8_t len;
    uint8_t data[wifi_cred_chunk_max_data];
}__packed;

// The commit carries the total lengths the sender intended, so a lost chunk
// is caught instead of saving a truncated password. ssid_len 0 clears the
// stored credentials.
struct wifi_cred_commit_t
{
    uint8_t ssid_len;
    uint8_t password_len;
}__packed;

// Stages one piece in RAM. A chunk for the SSID at offset 0 starts a new
// set, discarding anything staged before it.
void wifi_handle_cred_chunk(const wifi_cred_chunk_t *chunk);
// Validates the staged set, writes it to flash and signals core1 to
// reconnect with it.
void wifi_handle_cred_commit(const wifi_cred_commit_t *commit);

#endif
