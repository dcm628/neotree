#include "neo_tree_net_server.hpp"

#include <stdio.h>
#include <cstring>

#include "pico/cyw43_arch.h"
#include "lwip/tcp.h"
#include "lwip/ip_addr.h"
#include "lwip/apps/mdns.h"

#include "neo_tree_command_queue.hpp"
#include "neo_tree_protocol.hpp"

// Everything below except net_server_start()/net_server_poll() runs as lwIP
// callbacks, which in threadsafe_background mode execute in a low-priority
// IRQ on core1. No printf from there - it could block on the stdio mutex held
// by core1's own loop - so connection events go through event_log and are
// printed by net_server_poll() instead.

// Dead clients (a phone that went to sleep or left WiFi) are detected by TCP
// keepalive: first probe after 10s idle, then every 2s, dropped after 3
// unanswered - ~16s worst case.
static const u32_t keepalive_idle_ms = 10'000;
static const u32_t keepalive_interval_ms = 2'000;
static const u32_t keepalive_count = 3;

struct client_slot
{
    struct tcp_pcb *pcb;  // nullptr = free
    uint8_t header[2];    // length prefix being assembled
    uint8_t header_have;
    uint16_t msg_len;
    uint16_t msg_have;
    bool discarding;      // skipping an oversized message's payload
    uint64_t last_rx_us;
    uint8_t msg[command_max_len];
};

static client_slot clients[net_server_max_clients];
static struct tcp_pcb *listen_pcb = nullptr;
static volatile uint32_t commands_received = 0;

// ---- event log: single producer (lwIP IRQ) / single consumer (core1 loop),
// both on core1, so volatile indices are enough ----

enum class net_event_type : uint8_t
{
    CONNECTED,
    DISCONNECTED,
    EVICTED,   // slot taken over by a new client because all were in use
    ERRORED,   // connection reset / keepalive timeout
};

struct net_event
{
    net_event_type type;
    uint8_t slot;
    ip_addr_t addr;
    u16_t port;
};

static const uint32_t event_log_size = 16;
static net_event event_log[event_log_size];
static volatile uint32_t event_head = 0;  // written by IRQ
static volatile uint32_t event_tail = 0;  // written by loop

static void log_event(net_event_type type, uint8_t slot, const ip_addr_t *addr, u16_t port)
{
    uint32_t next = (event_head + 1) % event_log_size;
    if (next == event_tail)
    {
        return;  // full - drop rather than block in IRQ context
    }
    event_log[event_head].type = type;
    event_log[event_head].slot = slot;
    ip_addr_copy(event_log[event_head].addr, *addr);
    event_log[event_head].port = port;
    event_head = next;
}

// ---- helpers ----

static uint8_t slot_index(const client_slot *c)
{
    return static_cast<uint8_t>(c - clients);
}

static void send_frame(client_slot *c, const uint8_t *payload, uint16_t len)
{
    uint8_t frame[2 + 8];
    frame[0] = len & 0xFF;
    frame[1] = len >> 8;
    memcpy(frame + 2, payload, len);
    // A client that never reads eventually fills the send buffer; dropping
    // its replies is fine - it isn't listening anyway.
    tcp_write(c->pcb, frame, 2 + len, TCP_WRITE_FLAG_COPY);
}

static void send_ack(client_slot *c, uint8_t cmd_type, net_status status)
{
    uint8_t ack[3] = {static_cast<uint8_t>(net_reply_type::ACK), cmd_type, static_cast<uint8_t>(status)};
    send_frame(c, ack, sizeof(ack));
}

static void reset_slot(client_slot *c)
{
    c->pcb = nullptr;
    c->header_have = 0;
    c->msg_len = 0;
    c->msg_have = 0;
    c->discarding = false;
}

// Detaches callbacks and closes. Returns ERR_ABRT if it had to abort, which
// the caller must propagate if it's inside that pcb's recv callback.
static err_t close_client(client_slot *c)
{
    struct tcp_pcb *pcb = c->pcb;
    reset_slot(c);
    tcp_arg(pcb, nullptr);
    tcp_recv(pcb, nullptr);
    tcp_err(pcb, nullptr);
    if (tcp_close(pcb) != ERR_OK)
    {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    return ERR_OK;
}

static net_status handle_command(client_slot *c)
{
    uint8_t type = c->msg[0];
    if (type >= serial_msg_type_count)
    {
        return net_status::UNKNOWN_TYPE;
    }
    if (type == static_cast<uint8_t>(serial_msg_type::WIFI_CRED_CHUNK) ||
        type == static_cast<uint8_t>(serial_msg_type::WIFI_CRED_COMMIT))
    {
        return net_status::NOT_ALLOWED;
    }
    if (!protocol_msg_len_ok(c->msg, c->msg_len))
    {
        return net_status::BAD_LENGTH;
    }
    if (!command_queue_push(command_source::network, slot_index(c) + 1, c->msg, c->msg_len))
    {
        return net_status::QUEUE_FULL;
    }
    return net_status::QUEUED;
}

// Feeds received bytes through the length-prefix framing state machine.
static void consume_bytes(client_slot *c, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++)
    {
        if (c->header_have < 2)
        {
            c->header[c->header_have++] = data[i];
            if (c->header_have == 2)
            {
                c->msg_len = c->header[0] | (c->header[1] << 8);
                c->msg_have = 0;
                c->discarding = (c->msg_len == 0 || c->msg_len > command_max_len);
                if (c->msg_len == 0)
                {
                    send_ack(c, 0xFF, net_status::BAD_LENGTH);
                    c->header_have = 0;
                }
            }
            continue;
        }
        if (!c->discarding)
        {
            c->msg[c->msg_have] = data[i];
        }
        c->msg_have++;
        if (c->msg_have == c->msg_len)
        {
            if (c->discarding)
            {
                // Can't trust byte 0 of a message we didn't keep - report 0xFF.
                send_ack(c, 0xFF, net_status::BAD_LENGTH);
            }
            else
            {
                commands_received = commands_received + 1;
                send_ack(c, c->msg[0], handle_command(c));
            }
            c->header_have = 0;
        }
    }
}

// ---- lwIP callbacks ----

static void on_err(void *arg, err_t err)
{
    // lwIP has already freed the pcb.
    client_slot *c = static_cast<client_slot *>(arg);
    if (c != nullptr)
    {
        log_event(net_event_type::ERRORED, slot_index(c), IP_ADDR_ANY, 0);
        reset_slot(c);
    }
}

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    client_slot *c = static_cast<client_slot *>(arg);
    if (p == nullptr)
    {
        // Remote closed.
        log_event(net_event_type::DISCONNECTED, slot_index(c), &pcb->remote_ip, pcb->remote_port);
        return close_client(c);
    }
    if (err != ERR_OK)
    {
        pbuf_free(p);
        return err;
    }
    c->last_rx_us = time_us_64();
    for (struct pbuf *q = p; q != nullptr; q = q->next)
    {
        consume_bytes(c, static_cast<const uint8_t *>(q->payload), q->len);
    }
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    tcp_output(pcb);
    return ERR_OK;
}

static err_t on_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    if (err != ERR_OK || newpcb == nullptr)
    {
        return ERR_VAL;
    }
    client_slot *c = nullptr;
    for (auto &slot : clients)
    {
        if (slot.pcb == nullptr)
        {
            c = &slot;
            break;
        }
    }
    if (c == nullptr)
    {
        // All slots busy: most likely some are phones that went to sleep
        // and haven't timed out yet. Evict the one idle longest so a phone
        // that just woke up is never locked out.
        c = &clients[0];
        for (auto &slot : clients)
        {
            if (slot.last_rx_us < c->last_rx_us)
            {
                c = &slot;
            }
        }
        log_event(net_event_type::EVICTED, slot_index(c), &c->pcb->remote_ip, c->pcb->remote_port);
        struct tcp_pcb *old = c->pcb;
        reset_slot(c);
        tcp_arg(old, nullptr);
        tcp_recv(old, nullptr);
        tcp_err(old, nullptr);
        tcp_abort(old);
    }

    reset_slot(c);
    c->pcb = newpcb;
    c->last_rx_us = time_us_64();
    tcp_arg(newpcb, c);
    tcp_recv(newpcb, on_recv);
    tcp_err(newpcb, on_err);
    // Commands are tiny and latency matters more than packet count.
    tcp_nagle_disable(newpcb);
    newpcb->so_options |= SOF_KEEPALIVE;
    newpcb->keep_idle = keepalive_idle_ms;
    newpcb->keep_intvl = keepalive_interval_ms;
    newpcb->keep_cnt = keepalive_count;

    uint8_t hello[3] = {static_cast<uint8_t>(net_reply_type::HELLO), net_protocol_version,
                        static_cast<uint8_t>(tree_output_enabled ? hello_flag_output_on : 0)};
    send_frame(c, hello, sizeof(hello));
    tcp_output(newpcb);
    log_event(net_event_type::CONNECTED, slot_index(c), &newpcb->remote_ip, newpcb->remote_port);
    return ERR_OK;
}

// TXT record: lets a client check the protocol version before connecting.
static void mdns_txt(struct mdns_service *service, void *txt_userdata)
{
    char txt[16];
    int n = snprintf(txt, sizeof(txt), "proto=%u", net_protocol_version);
    mdns_resp_add_service_txtitem(service, txt, static_cast<u8_t>(n));
}

// ---- public ----

bool net_server_start()
{
    for (auto &slot : clients)
    {
        reset_slot(&slot);
    }
    cyw43_arch_lwip_begin();
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    bool ok = pcb != nullptr && tcp_bind(pcb, IP_ANY_TYPE, net_server_port) == ERR_OK;
    if (ok)
    {
        listen_pcb = tcp_listen_with_backlog(pcb, net_server_max_clients);
        ok = listen_pcb != nullptr;
        if (ok)
        {
            tcp_accept(listen_pcb, on_accept);
        }
    }
    else if (pcb != nullptr)
    {
        tcp_close(pcb);
    }
    // Discovery: <hostname>.local plus a _neotree._tcp service record that
    // points at the command port. Announces automatically each time the link
    // comes up (MDNS_RESP_USENETIF_EXTCALLBACK).
    bool mdns_ok = false;
    if (ok)
    {
        struct netif *sta = &cyw43_state.netif[CYW43_ITF_STA];
        mdns_resp_init();
        mdns_ok = mdns_resp_add_netif(sta, CYW43_HOST_NAME) == ERR_OK &&
                  mdns_resp_add_service(sta, "neotree", "_neotree", DNSSD_PROTO_TCP, net_server_port,
                                        mdns_txt, nullptr) >= 0;
    }
    cyw43_arch_lwip_end();
    printf("net: command server %s on port %u, mDNS %s (%s.local, _neotree._tcp)\n",
           ok ? "listening" : "FAILED to start", net_server_port, mdns_ok ? "on" : "FAILED", CYW43_HOST_NAME);
    return ok;
}

void net_server_poll()
{
    while (event_tail != event_head)
    {
        const net_event &e = event_log[event_tail];
        const char *what = "?";
        switch (e.type)
        {
        case net_event_type::CONNECTED: what = "connected"; break;
        case net_event_type::DISCONNECTED: what = "disconnected"; break;
        case net_event_type::EVICTED: what = "evicted (all slots busy, idle longest)"; break;
        case net_event_type::ERRORED: what = "dropped (reset or keepalive timeout)"; break;
        }
        if (e.type == net_event_type::ERRORED)
        {
            printf("net: client slot %u %s\n", e.slot, what);
        }
        else
        {
            printf("net: client %s:%u slot %u %s\n", ipaddr_ntoa(&e.addr), e.port, e.slot, what);
        }
        event_tail = (event_tail + 1) % event_log_size;
    }
}

uint32_t net_server_client_count()
{
    uint32_t n = 0;
    for (auto &slot : clients)
    {
        n += (slot.pcb != nullptr);
    }
    return n;
}

uint32_t net_server_commands_received()
{
    return commands_received;
}
