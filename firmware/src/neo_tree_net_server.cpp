#include "neo_tree_net_server.hpp"

#include <stdio.h>
#include <cstring>

#include "pico/cyw43_arch.h"
#include "lwip/tcp.h"
#include "lwip/ip_addr.h"
#include "lwip/apps/mdns.h"
#include "lwip/memp.h"
#include "lwip/stats.h"

#include "neo_tree_command_queue.hpp"
#include "neo_tree_protocol.hpp"
#include "neo_tree_status.hpp"
#include "neo_tree_event_log.hpp"

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

// Room for ~25 queued ACKs - far more than a client waiting on each reply can
// ever have outstanding.
static const size_t pending_max = 128;

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
    // Replies lwIP couldn't take yet (tcp_write ERR_MEM), in order, retried
    // from the sent/poll callbacks. A dropped reply would desync the client -
    // it pairs each ACK with its command - so they're never dropped: if this
    // overflows the client isn't reading, and the connection is closed.
    uint8_t pending[pending_max];
    uint16_t pending_len;
    bool overflowed;
};

static client_slot clients[net_server_max_clients];
static struct tcp_pcb *listen_pcb = nullptr;
static volatile uint32_t commands_received = 0;
static volatile net_server_diag_t diag = {};

// ---- event log: single producer (lwIP IRQ) / single consumer (core1 loop),
// both on core1, so volatile indices are enough ----

enum class net_event_type : uint8_t
{
    CONNECTED,
    DISCONNECTED,
    EVICTED,   // slot taken over by a new client because all were in use
    ERRORED,   // connection reset / keepalive timeout
    OVERFLOWED,   // closed: its reply backlog overflowed (client not reading)
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

// Hands the client's pending bytes to lwIP. tcp_write is all-or-nothing, so
// on ERR_MEM they simply stay pending for the next attempt.
static void write_pending(client_slot *c)
{
    if (c->pending_len == 0)
    {
        return;
    }
    err_t err = tcp_write(c->pcb, c->pending, c->pending_len, TCP_WRITE_FLAG_COPY);
    if (err == ERR_OK)
    {
        c->pending_len = 0;
    }
    else if (err != ERR_MEM)
    {
        diag.write_failures = diag.write_failures + 1;
        diag.last_write_error = err;
    }
}

static void flush(struct tcp_pcb *pcb)
{
    if (tcp_output(pcb) != ERR_OK)
    {
        diag.output_failures = diag.output_failures + 1;
    }
}

static void send_frame(client_slot *c, const uint8_t *payload, uint16_t len)
{
    uint8_t frame[2 + 8];
    frame[0] = len & 0xFF;
    frame[1] = len >> 8;
    memcpy(frame + 2, payload, len);
    // Straight to lwIP unless earlier replies are still waiting (order must
    // be kept) or lwIP is out of memory; then queue it for write_pending().
    if (c->pending_len == 0)
    {
        err_t err = tcp_write(c->pcb, frame, 2 + len, TCP_WRITE_FLAG_COPY);
        if (err == ERR_OK)
        {
            return;
        }
        if (err != ERR_MEM)
        {
            diag.write_failures = diag.write_failures + 1;
            diag.last_write_error = err;
            return;
        }
    }
    diag.writes_deferred = diag.writes_deferred + 1;
    if (c->pending_len + 2 + len > pending_max)
    {
        c->overflowed = true;   // closed by the next callback that can safely do it
        return;
    }
    memcpy(c->pending + c->pending_len, frame, 2 + len);
    c->pending_len += 2 + len;
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
    c->pending_len = 0;
    c->overflowed = false;
}

static void detach_callbacks(struct tcp_pcb *pcb)
{
    tcp_arg(pcb, nullptr);
    tcp_recv(pcb, nullptr);
    tcp_err(pcb, nullptr);
    tcp_sent(pcb, nullptr);
    tcp_poll(pcb, nullptr, 0);
}

// Detaches callbacks and closes. Returns ERR_ABRT if it had to abort, which
// the caller must propagate if it's inside one of that pcb's callbacks.
static err_t close_client(client_slot *c)
{
    struct tcp_pcb *pcb = c->pcb;
    reset_slot(c);
    detach_callbacks(pcb);
    if (tcp_close(pcb) != ERR_OK)
    {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    return ERR_OK;
}

// A client whose reply backlog overflowed isn't reading - drop it (it can
// reconnect) rather than silently lose replies it would pair with commands.
// Call only from one of its pcb's callbacks; returns ERR_ABRT for them to
// propagate.
static err_t drop_overflowed_client(client_slot *c)
{
    diag.overflow_closes = diag.overflow_closes + 1;
    log_event(net_event_type::OVERFLOWED, slot_index(c), &c->pcb->remote_ip, c->pcb->remote_port);
    struct tcp_pcb *pcb = c->pcb;
    reset_slot(c);
    detach_callbacks(pcb);
    tcp_abort(pcb);
    return ERR_ABRT;
}

// Answers a STATUS_REQUEST with a STATUS frame ([len][0x82][JSON]) right here
// on core1 - it doesn't touch core0's state machine, so it isn't queued. One
// all-or-nothing tcp_write so a partial frame can never corrupt the stream;
// if it can't go now it's dropped (the client polls again), and skipped
// while ACKs are backlogged so it can't starve them.
static void send_status(client_slot *c)
{
    static uint8_t frame[3 + status_json_max];   // static: IRQ context, one core
    size_t json_len = status_build_json(reinterpret_cast<char *>(frame + 3), status_json_max);
    size_t payload_len = 1 + json_len;
    frame[0] = payload_len & 0xFF;
    frame[1] = payload_len >> 8;
    frame[2] = static_cast<uint8_t>(net_reply_type::STATUS);
    if (c->pending_len > 0 || tcp_write(c->pcb, frame, 2 + payload_len, TCP_WRITE_FLAG_COPY) != ERR_OK)
    {
        diag.status_dropped = diag.status_dropped + 1;
    }
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
    if (type == static_cast<uint8_t>(serial_msg_type::STATUS_REQUEST))
    {
        send_status(c);   // then the usual ACK, so every command still gets one
        return net_status::QUEUED;
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
    if (c->overflowed)
    {
        return drop_overflowed_client(c);
    }
    flush(pcb);
    return ERR_OK;
}

// Retries replies deferred by ERR_MEM: on_sent fires as the client
// acknowledges data (freeing lwIP memory), on_poll every second as a backstop.
static err_t retry_pending(void *arg, struct tcp_pcb *pcb)
{
    client_slot *c = static_cast<client_slot *>(arg);
    if (c == nullptr)
    {
        return ERR_OK;
    }
    if (c->overflowed)
    {
        return drop_overflowed_client(c);
    }
    if (c->pending_len > 0)
    {
        write_pending(c);
        flush(pcb);
    }
    return ERR_OK;
}

static err_t on_sent(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    return retry_pending(arg, pcb);
}

static err_t on_poll(void *arg, struct tcp_pcb *pcb)
{
    return retry_pending(arg, pcb);
}

static err_t on_accept(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    if (err != ERR_OK || newpcb == nullptr)
    {
        diag.accept_errors = diag.accept_errors + 1;
        return ERR_VAL;
    }
    diag.accepts = diag.accepts + 1;
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
        detach_callbacks(old);
        tcp_abort(old);
    }

    reset_slot(c);
    c->pcb = newpcb;
    c->last_rx_us = time_us_64();
    tcp_arg(newpcb, c);
    tcp_recv(newpcb, on_recv);
    tcp_err(newpcb, on_err);
    tcp_sent(newpcb, on_sent);
    tcp_poll(newpcb, on_poll, 2);   // interval in 500ms ticks
    // Commands are tiny and latency matters more than packet count.
    tcp_nagle_disable(newpcb);
    newpcb->so_options |= SOF_KEEPALIVE;
    newpcb->keep_idle = keepalive_idle_ms;
    newpcb->keep_intvl = keepalive_interval_ms;
    newpcb->keep_cnt = keepalive_count;

    uint8_t hello[3] = {static_cast<uint8_t>(net_reply_type::HELLO), net_protocol_version,
                        static_cast<uint8_t>(tree_output_enabled ? hello_flag_output_on : 0)};
    send_frame(c, hello, sizeof(hello));
    flush(newpcb);
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
        case net_event_type::OVERFLOWED: what = "closed (not reading replies - backlog overflowed)"; break;
        }
        if (e.type == net_event_type::ERRORED)
        {
            event_logf("net: client slot %u %s", e.slot, what);
        }
        else
        {
            event_logf("net: client %s:%u slot %u %s", ipaddr_ntoa(&e.addr), e.port, e.slot, what);
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

net_server_diag_t net_server_diag()
{
    net_server_diag_t d;
    d.accepts = diag.accepts;
    d.accept_errors = diag.accept_errors;
    d.write_failures = diag.write_failures;
    d.output_failures = diag.output_failures;
    d.last_write_error = diag.last_write_error;
    d.writes_deferred = diag.writes_deferred;
    d.overflow_closes = diag.overflow_closes;
    d.status_dropped = diag.status_dropped;
    return d;
}

void net_server_print_lwip_stats()
{
    // Read without the lwIP lock: a slightly torn snapshot is fine for a
    // diagnostic line, and it keeps the heartbeat from stalling the stack.
    const struct stats_mem &heap = lwip_stats.mem;
    const struct stats_mem *seg = lwip_stats.memp[MEMP_TCP_SEG];
    const struct stats_mem *pcb = lwip_stats.memp[MEMP_TCP_PCB];
    const struct stats_mem *pool = lwip_stats.memp[MEMP_PBUF_POOL];
    printf("lwip: heap used %u max %u of %u err %u | tcp_seg used %u max %u of %u err %u | "
           "tcp_pcb max %u of %u err %u | pbuf_pool max %u of %u err %u\n",
           (unsigned)heap.used, (unsigned)heap.max, (unsigned)heap.avail, (unsigned)heap.err,
           (unsigned)seg->used, (unsigned)seg->max, (unsigned)seg->avail, (unsigned)seg->err,
           (unsigned)pcb->max, (unsigned)pcb->avail, (unsigned)pcb->err,
           (unsigned)pool->max, (unsigned)pool->avail, (unsigned)pool->err);
}
