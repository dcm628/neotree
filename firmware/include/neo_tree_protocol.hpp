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
    // Global lights on/off: [type][1 = on, 0 = off]. Off sends zeros to every
    // LED each frame without touching any LED's base/overlay colors, so on
    // restores exactly what was showing.
    TREE_OUTPUT,
    // Diagnostic: [type][led_output_mode] - how frames are sent to the
    // strings (neo_tree_led_output.hpp). Not persisted; boots in PARALLEL_DMA.
    LED_OUTPUT_MODE,
    // Diagnostic: [type][fast_slew 0/1][drive 0-3 = 2/4/8/12mA][stagger in
    // 10ns units][frame rate fps, 0 = unchanged] - output tuning
    // (led_output_tuning_t + the frame rate target). Not persisted.
    LED_OUTPUT_TUNING,
    // [type]: JSON snapshot of the tree's internals (neo_tree_status.hpp) -
    // over the network a STATUS reply frame, over USB serial a "status:" line.
    STATUS_REQUEST,
    // [type]: reboot via the watchdog (~250ms after, so the ACK gets out).
    REBOOT,
    // [type]: leave the WiFi network and rejoin.
    WIFI_RECONNECT,
    // Reboot into BOOTSEL (USB mass-storage flashing) - the network escape
    // hatch for when USB serial is unavailable. No payload.
    BOOTSEL,
    // Runs one of the former demos (now modes) in slot 1, above the Canvas:
    // [22][demo id]. 0 = none (back to the Canvas).
    DEMO,
    // Modes (engine/include/neotree/modes.hpp, director.hpp). Mode and
    // preset indices come from DESCRIBE.
    DESCRIBE,       // 23: -> [0x83][JSON]: modes, their parameters, presets
    SLOT_SET,       // 24: [slot][mode index, 0xFF = empty][transition 0 cut / 1 fade]
    PARAM_SET,      // 25: [slot][param][f32 LE value][r][g][b]
    SLOT_END,       // 26: [slot][op: 0 end (its policy), 1 revert, 2 remove, 3 restart]; slot 0xFF + op 1 = revert the scene
    SLOT_LIFE,      // 27: [slot][u16 LE duration s][u16 LE cycles][policy][repeats][next mode index][transition][transition x0.1 s]
    INPUT,          // 28: [slot][id][f32 LE value] - an input event for the slot's rules
    PRESET,         // 29: [preset index] (built-ins, then the user's - see LIBRARY)
    // The library (engine/include/neotree/library.hpp): presets, shows, the
    // base scene. Preset and show indices come from LIBRARY; names are 20
    // bytes, NUL-padded. Changes are stored in flash (neo_tree_scene_store).
    LIBRARY,        // 30: -> [0x85][JSON]: presets, shows, base scene, startup show
    SCENE_SAVE,     // 31: [0 = as the base scene, 1 = as a preset][name] - saves the live scene; a preset replaces the user preset of that name
    LIBRARY_DELETE, // 32: [0 = base scene (back to the default), 1 = preset, 2 = show][index] - user items only
    SHOW_SET,       // 33: [flags: bit 0 loop, bit 1 shuffle][count][name][(preset index, u16 LE seconds) x count] - saves a user show (replaces by name)
    SHOW_PLAY,      // 34: [show index, 0xFF = stop (the scene stays)]
    SHOW_BOOT,      // 35: [show index, 0xFF = none] - the show to play at power-up
    // Network only: [flags: bit 0 scene, bit 1 library] - this connection is
    // sent SCENE / LIBRARY frames whenever they change (and once now).
    SUBSCRIBE,      // 36
};

// Number of defined command types - anything >= this is unknown.
const uint8_t serial_msg_type_count = static_cast<uint8_t>(serial_msg_type::SUBSCRIBE) + 1;

// SCENE_SAVE / SHOW_SET name field size (neotree::name_size).
const size_t protocol_name_len = 20;
const uint8_t protocol_max_show_entries = 16;

// Set by TREE_OUTPUT (core0), read by the LED output path and reported to
// network clients in HELLO (core1).
extern volatile bool tree_output_enabled;

// True if len is a valid size for this message (byte 0 = type). Implemented
// in main.cpp, next to the frame structs it checks against.
bool protocol_msg_len_ok(const uint8_t *msg, size_t len);

#endif
