#ifndef _NEO_TREE_NET_SERVER_HPP
#define _NEO_TREE_NET_SERVER_HPP

#include "pico/stdlib.h"

// TCP command server: lets clients on the LAN (the Android app, Python
// tools) send the same command messages as USB serial.
//
// Wire format, both directions: [uint16 little-endian length][payload].
// Client -> tree: payload is a command message exactly as sent over serial
//   (byte 0 = serial_msg_type, see neo_tree_protocol.hpp). The server checks
//   it and pushes it onto the shared command queue.
// Tree -> client: byte 0 >= 0x80 identifies the reply:
//   HELLO  [0x81][protocol_version][flags][stream token, u32 LE]   sent once on connect
//          flags bit 0: lights on (TREE_OUTPUT). The token identifies this
//          connection on the UDP stream (below). Older clients that only read
//          the first bytes are unaffected.
//   ACK    [0x80][command type][net_status]    one per received command
//   STATUS [0x82][JSON]                        reply to STATUS_REQUEST (neo_tree_status.hpp),
//                                              sent just before that command's ACK
//   DESCRIBE [0x83][JSON]                      reply to DESCRIBE: the modes
//   SCENE  [0x84][JSON]                        the running scene, pushed to SUBSCRIBEd clients
//                                              when it changes (not as time passes)
//   LIBRARY [0x85][JSON]                       presets, shows and effects: reply to LIBRARY,
//                                              and pushed to SUBSCRIBEd clients when it changes
//   FX_SCHEMA [0x86][JSON]                     reply to FX_SCHEMA: a section of the effect schema
//   FX     [0x87][JSON]                        reply to FX_GET: a section of the draft effect,
//                                              a frame or so after that command's ACK
//   SCHEDULE [0x88][JSON]                      the schedule: reply to SCHEDULE, and sent after
//                                              every LIBRARY frame (reply or push)
// ACK means "accepted onto the command queue", not "already applied".
//
// Stream channel: UDP port net_stream_port, for high-rate input where only
// the newest matters (docs/ARCHITECTURE.md 7). Each datagram is
//   ['N'][1][token u32 LE][seq u16 LE][message]
// with the token from this client's HELLO; the message is a BRUSH command.
// Nothing is sent back. Datagrams older than the newest seen (by seq) are
// dropped, as are unknown tokens; losing one only means a coarser stroke.
// Pushed frames can arrive between a command and its ACK; clients that
// don't SUBSCRIBE never get them.

const uint16_t net_server_port = 7777;
const uint16_t net_stream_port = 7778;
// Largest JSON reply frame: DESCRIBE (the modes and their parameters - 3.8 KB
// with 14 modes) can outgrow the 4 KB status report, so frames have room.
const size_t net_reply_json_max = 6144;
const uint8_t net_protocol_version = 1;
const size_t net_server_max_clients = 4;
const uint8_t hello_flag_output_on = 0x01;

enum class net_reply_type : uint8_t
{
    ACK = 0x80,
    HELLO = 0x81,
    STATUS = 0x82,   // [0x82][JSON] - reply to STATUS_REQUEST, sent before its ACK
    DESCRIBE = 0x83, // [0x83][JSON] - reply to DESCRIBE: modes and presets
    SCENE = 0x84,    // [0x84][JSON] - pushed: the scene (Director::describe_state)
    LIBRARY = 0x85,  // [0x85][JSON] - reply to LIBRARY, and pushed (Library::describe)
    FX_SCHEMA = 0x86, // [0x86][JSON] - reply to FX_SCHEMA (effect_schema_json)
    FX = 0x87,       // [0x87][JSON] - reply to FX_GET (effect_section_json of the draft)
    SCHEDULE = 0x88, // [0x88][JSON] - reply to SCHEDULE and after LIBRARY (Library::describe_schedule)
};

enum class net_status : uint8_t
{
    QUEUED = 0,
    QUEUE_FULL = 1,
    UNKNOWN_TYPE = 2,
    BAD_LENGTH = 3,
    NOT_ALLOWED = 4,  // e.g. WiFi credentials - USB serial only
};

// core1, after cyw43_arch_init(). Listens on all interfaces, so it can start
// before the WiFi link is up.
bool net_server_start();

// core1 loop: prints connection events (connect/disconnect/evict/drop)
// logged by the lwIP callbacks, which can't print themselves.
void net_server_poll();

// For the heartbeat.
uint32_t net_server_client_count();
uint32_t net_server_commands_received();

// Cumulative since boot, for diagnosing connection problems.
struct net_server_diag_t
{
    uint32_t accepts;
    uint32_t accept_errors;     // lwIP handed on_accept an error (e.g. out of memory)
    uint32_t write_failures;    // tcp_write failed with something other than ERR_MEM - frame dropped
    uint32_t writes_deferred;   // frames queued because lwIP was out of memory (retried, not lost)
    uint32_t overflow_closes;   // clients closed because their reply backlog overflowed
    uint32_t status_dropped;    // STATUS replies not sent (lwIP out of memory, or replies backlogged)
    uint32_t output_failures;   // tcp_output failed
    int32_t last_write_error;   // lwIP err_t of the most recent write failure
    uint32_t stream_rx;         // stream datagrams accepted
    uint32_t stream_old;        // ... dropped as older than one already seen
    uint32_t stream_bad;        // ... dropped as malformed or with an unknown token
};
net_server_diag_t net_server_diag();
// One heartbeat line of lwIP heap/pool usage and allocation failures.
void net_server_print_lwip_stats();

#endif
