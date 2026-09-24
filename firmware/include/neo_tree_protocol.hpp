#ifndef _NEO_TREE_PROTOCOL_HPP
#define _NEO_TREE_PROTOCOL_HPP

#include "pico/stdlib.h"

// Command messages, shared by every transport. Byte 0 of a message is its
// serial_msg_type; the rest is the matching *_frame payload (main.cpp).
// Over USB serial a message is one unframed read burst; over the network
// (neo_tree_net_server.hpp) each is prefixed with a uint16 little-endian
// length.
enum class serial_msg_type : uint8_t
{
    NOOP,
    SINGLE_LED_UPDATE,
    COLOR_GROUP_RGB_UPDATE,
    ALL_LED_UPDATE,
    LED_POS_UPDATE_CARTESIAN,
    LED_POS_UPDATE_CYLINDRICAL,
    CONFIG_RELOAD,
    RUN_SWEEP_SEQUENCE,
    // Appended rather than inserted, to keep existing numeric values
    // (and therefore wire compatibility) unchanged.
    READ_POS_CONFIG,
    SET_VOLUME_CARTESIAN,
    SET_VOLUME_CYLINDRICAL,
    // One-shot: overwrites both flash and the live tree with the
    // compiled-in default position config (mapping/
    // generate_pos_config_header.py) - the fast path for pushing a full
    // coordinate update (reflash with freshly generated data, then send
    // this) instead of replaying ~1000 individual position writes.
    RESET_POS_CONFIG_TO_DEFAULT,
    // Sets the PRIMARY/base color for every LED (same payload shape as
    // ALL_LED_UPDATE, which only ever touches the secondary overlay) - the
    // color shown when a LED isn't currently lit by anything else, e.g.
    // outside a SET_VOLUME_* window with clear_outside_volume set.
    ALL_LED_UPDATE_BASE,
    // WiFi provisioning (tools/set_wifi.py): credentials sent in <=32-byte
    // pieces, then a commit that writes them to flash - see neo_tree_wifi.hpp.
    WIFI_CRED_CHUNK,
    WIFI_CRED_COMMIT,
};

// Number of defined command types - anything >= this is unknown.
const uint8_t serial_msg_type_count = static_cast<uint8_t>(serial_msg_type::WIFI_CRED_COMMIT) + 1;

// True if len is a valid size for this message (byte 0 = type). Implemented
// in main.cpp, next to the frame structs it checks against.
bool protocol_msg_len_ok(const uint8_t *msg, size_t len);

#endif
