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
//   HELLO  [0x81][protocol_version]            sent once on connect
//   ACK    [0x80][command type][net_status]    one per received command
// ACK means "accepted onto the command queue", not "already applied".

const uint16_t net_server_port = 7777;
const uint8_t net_protocol_version = 1;
const size_t net_server_max_clients = 4;

enum class net_reply_type : uint8_t
{
    ACK = 0x80,
    HELLO = 0x81,
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

#endif
