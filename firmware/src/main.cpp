/*  This program is my neopixel Christmas Tree running from a Pico W 
    It has a bunch of hardcoding of things and is not meant to be a general purpose program. */

// normie files
#include <stdio.h>
#include <stdlib.h>
// RPi files
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "pico/multicore.h"
#include "pico/cyw43_arch.h"
// my code files
#include "dcm_rgb.hpp"
#include "neo_tree_config.hpp"
#include "dcm_physics_math.hpp"
#include "neo_tree_wifi.hpp"
#include "neo_tree_command_queue.hpp"
#include "neo_tree_protocol.hpp"
#include "neo_tree_net_server.hpp"
#include "neo_tree_led_output.hpp"
#include "neo_tree_event_log.hpp"
#include "neo_tree_status.hpp"
#include "neo_tree_engine.hpp"
#include "neo_tree_loop_monitor.hpp"
#include "neo_tree_safety.hpp"
#include "neotree/modes.hpp"

mutex core0_data_update;


volatile bool tree_output_enabled = true;
// LED frame rate target. Defaults to NEOTREE_FRAME_RATE from CMake (60);
// adjustable live with LED_OUTPUT_TUNING. Above ~107 frames just run back to
// back, limited by the output itself (~9.3ms per frame).
volatile uint32_t target_loop_rate = NEOTREE_FRAME_RATE;
// core1 main-loop iterations, for the status snapshot (its rate shows core1 load).
volatile uint32_t core1_loop_counter = 0;

uint8_t sleep_val = 25;
// Was 40 - too small to hold a COLOR_GROUP_RGB_UPDATE message (up to
// max_group_update_entries LEDs at 5 bytes each, see dcm_rgb.hpp). Every
// message type shares this one fixed-size buffer, so bumping it just gives
// the smaller messages more headroom - trivial extra RAM (2 buffers x a
// few hundred bytes) on an RP2040.
#define SERIAL_BUFFER_SIZE command_max_len
// core1-only: accumulates one serial read burst.
uint8_t serial_buf[SERIAL_BUFFER_SIZE] = {};
// core0-only: the message process_msg() is currently handling, copied out of
// the command queue (zero-padded past its length).
uint8_t serial_buf_copy[SERIAL_BUFFER_SIZE] = {};
size_t buf_index = 0;   // size_t: a uint8_t wrapped to 0 on a 256-byte burst
int16_t temp_char = -1; // init to a no bytes value
// Runs on core1. Serial has no framing, so each read burst is treated as one
// message (see max_group_update_entries in dcm_rgb.hpp for why senders keep
// messages within one USB packet).
void serial_read_buffer()
{
    while (buf_index < SERIAL_BUFFER_SIZE && (temp_char = getchar_timeout_us(0)) >= 0)
    {
        //add to buffer
        serial_buf[buf_index] = temp_char;
        buf_index++;
    }
    if (buf_index != 0)
    {
        // Don't echo credential bytes back to whoever has the port open.
        if (serial_buf[0] != static_cast<uint8_t>(serial_msg_type::WIFI_CRED_CHUNK))
        {
            for (size_t i = 0; i < buf_index; i++)
            {
                printf("%d\n", serial_buf[i]);
            }
        }
        if (!command_queue_push(command_source::usb_serial, 0, serial_buf, buf_index))
        {
            printf("command queue full - dropped serial message type %d\n", serial_buf[0]);
        }
        memset(serial_buf, 0, SERIAL_BUFFER_SIZE);
        buf_index = 0;
    }
}
uint32_t msg_process_counter = 0;
serial_msg_type new_msg = serial_msg_type::NOOP;
struct single_led_update_frame
{
    uint8_t s_msg_type;
    single_led_update_t s_msg;
}__packed;
struct all_led_update_frame
{
    uint8_t s_msg_type;
    all_led_update_t s_msg;
}__packed;
struct single_led_pos_cylindrical_update_frame
{
    uint8_t s_msg_type;
    single_led_pos_cylindrical_update_t s_msg;
}__packed;
struct single_led_pos_cartesian_update_frame
{
    uint8_t s_msg_type;
    single_led_pos_cartesian_update_t s_msg;
}__packed;
struct config_reload_frame
{
    uint8_t s_msg_type;
    config_type s_msg;
}__packed;
struct read_pos_config_request_frame
{
    uint8_t s_msg_type;
    read_pos_config_request_t s_msg;
}__packed;
struct group_led_update_frame
{
    uint8_t s_msg_type;
    group_led_update_t s_msg;
}__packed;
struct set_volume_cartesian_frame
{
    uint8_t s_msg_type;
    set_volume_cartesian_t s_msg;
}__packed;
struct set_volume_cylindrical_frame
{
    uint8_t s_msg_type;
    set_volume_cylindrical_t s_msg;
}__packed;

struct wifi_cred_chunk_frame
{
    uint8_t s_msg_type;
    wifi_cred_chunk_t s_msg;
}__packed;
struct wifi_cred_commit_frame
{
    uint8_t s_msg_type;
    wifi_cred_commit_t s_msg;
}__packed;

bool protocol_msg_len_ok(const uint8_t *msg, size_t len)
{
    if (len == 0)
    {
        return false;
    }
    switch (static_cast<serial_msg_type>(msg[0]))
    {
    case serial_msg_type::NOOP:
    case serial_msg_type::RESET_POS_CONFIG_TO_DEFAULT:
        return len == 1;
    case serial_msg_type::SINGLE_LED_UPDATE:
        return len == sizeof(single_led_update_frame);
    case serial_msg_type::COLOR_GROUP_RGB_UPDATE:
    {
        // Variable length: type, count, then count entries.
        if (len < 2 || msg[1] > max_group_update_entries)
        {
            return false;
        }
        return len == 2 + msg[1] * sizeof(group_led_entry_t);
    }
    case serial_msg_type::ALL_LED_UPDATE:
    case serial_msg_type::ALL_LED_UPDATE_BASE:
        return len == sizeof(all_led_update_frame);
    case serial_msg_type::LED_POS_UPDATE_CARTESIAN:
        return len == sizeof(single_led_pos_cartesian_update_frame);
    case serial_msg_type::LED_POS_UPDATE_CYLINDRICAL:
        return len == sizeof(single_led_pos_cylindrical_update_frame);
    case serial_msg_type::CONFIG_RELOAD:
        return len == sizeof(config_reload_frame);
    case serial_msg_type::READ_POS_CONFIG:
        return len == sizeof(read_pos_config_request_frame);
    case serial_msg_type::SET_VOLUME_CARTESIAN:
        return len == sizeof(set_volume_cartesian_frame);
    case serial_msg_type::SET_VOLUME_CYLINDRICAL:
        return len == sizeof(set_volume_cylindrical_frame);
    case serial_msg_type::WIFI_CRED_CHUNK:
        return len == sizeof(wifi_cred_chunk_frame);
    case serial_msg_type::WIFI_CRED_COMMIT:
        return len == sizeof(wifi_cred_commit_frame);
    case serial_msg_type::TREE_OUTPUT:
    case serial_msg_type::LED_OUTPUT_MODE:
        return len == 2;
    case serial_msg_type::LED_OUTPUT_TUNING:
        return len == 6;
    case serial_msg_type::STATUS_REQUEST:
    case serial_msg_type::REBOOT:
    case serial_msg_type::WIFI_RECONNECT:
    case serial_msg_type::BOOTSEL:
        return len == 1;
    case serial_msg_type::DEMO:
    case serial_msg_type::PRESET:
        return len == 2;
    case serial_msg_type::DESCRIBE:
        return len == 1;
    case serial_msg_type::SLOT_SET:
    case serial_msg_type::SLOT_END:
        return len == (msg[0] == static_cast<uint8_t>(serial_msg_type::SLOT_SET) ? 4u : 3u);
    case serial_msg_type::PARAM_SET:
        return len == 10;
    case serial_msg_type::SLOT_LIFE:
        return len == 11;
    case serial_msg_type::INPUT:
        return len == 7;
    default:
        // Includes RUN_SWEEP_SEQUENCE, which process_msg() never implemented.
        return false;
    }
}

union single_led_update_msg
{
    uint8_t buf[SERIAL_BUFFER_SIZE];
    single_led_update_frame msg;
};
union all_led_update_msg
{
    uint8_t buf[SERIAL_BUFFER_SIZE];
    all_led_update_frame msg;
};
union single_led_pos_cylindrical_update_msg
{
    uint8_t buf[SERIAL_BUFFER_SIZE];
    single_led_pos_cylindrical_update_frame msg;
};
union single_led_pos_cartesian_update_msg
{
    uint8_t buf[SERIAL_BUFFER_SIZE];
    single_led_pos_cartesian_update_frame msg;
};
union config_reload_msg
{
    uint8_t buf[SERIAL_BUFFER_SIZE];
    config_reload_frame msg;
};
union read_pos_config_request_msg
{
    uint8_t buf[SERIAL_BUFFER_SIZE];
    read_pos_config_request_frame msg;
};
union group_led_update_msg
{
    uint8_t buf[SERIAL_BUFFER_SIZE];
    group_led_update_frame msg;
};
union set_volume_cartesian_msg
{
    uint8_t buf[SERIAL_BUFFER_SIZE];
    set_volume_cartesian_frame msg;
};
union set_volume_cylindrical_msg
{
    uint8_t buf[SERIAL_BUFFER_SIZE];
    set_volume_cylindrical_frame msg;
};

void process_msg()
{
    // core0 only - handles the message the main loop just copied into
    // serial_buf_copy from the command queue. First byte is msg_type
    new_msg = static_cast<serial_msg_type>(serial_buf_copy[0]);
/*     if (new_msg == serial_msg_type::SINGLE_LED_UPDATE)
    {
        update_msg temp_update_msg;
        memcpy(temp_update_msg.buf,serial_buf_copy,sizeof(temp_update_msg.buf));    // extra copy fuck it - it works
        //*temp_update_msg.buf = *serial_buf_copy;
        // do update stuff
        RGB_LED_3D::update_single(&(temp_update_msg.msg.s_msg));
        new_msg = serial_msg_type::NOOP;
        msg_process_counter++;
    }
    else if (new_msg == serial_msg_type::COLOR_GROUP_RGB_UPDATE)
    {
        // don't do shit yet
    }
 */    
    // shouldn't need this, just cast the buffer but w/e fuck it

    single_led_update_msg temp_update_msg;
    all_led_update_msg temp_all_led_update_msg;
    single_led_pos_cylindrical_update_msg temp_single_led_pos_cylindrical_update_msg;
    single_led_pos_cartesian_update_msg temp_single_led_pos_cartesian_update_msg;
    config_reload_msg temp_config_reload_msg;
    read_pos_config_request_msg temp_read_pos_config_msg;
    group_led_update_msg temp_group_led_update_msg;
    set_volume_cartesian_msg temp_set_volume_cartesian_msg;
    set_volume_cylindrical_msg temp_set_volume_cylindrical_msg;
    switch (new_msg)
    {
    case serial_msg_type::NOOP:
        // Don't need to do shit, but can do a serial print to show it worked
        printf("NOOP received ");
        break;
    case serial_msg_type::SINGLE_LED_UPDATE:
        memcpy(temp_update_msg.buf,serial_buf_copy,sizeof(temp_update_msg.buf));    // extra copy fuck it - it works
        engine_host_canvas_single(&(temp_update_msg.msg.s_msg));
        new_msg = serial_msg_type::NOOP;
        msg_process_counter++;
        break;
    case serial_msg_type::COLOR_GROUP_RGB_UPDATE:
        memcpy(temp_group_led_update_msg.buf,serial_buf_copy,sizeof(temp_update_msg.buf));    // extra copy fuck it - it works
        engine_host_canvas_group(&(temp_group_led_update_msg.msg.s_msg));
        new_msg = serial_msg_type::NOOP;
        msg_process_counter++;
        break;
    case serial_msg_type::ALL_LED_UPDATE:
        memcpy(temp_all_led_update_msg.buf,serial_buf_copy,sizeof(temp_update_msg.buf));    // extra copy fuck it - it works
        engine_host_canvas_fill(&(temp_all_led_update_msg.msg.s_msg));
        new_msg = serial_msg_type::NOOP;
        msg_process_counter++;
        break;
    case serial_msg_type::LED_POS_UPDATE_CARTESIAN:
        printf("LED_POS_UPDATE_CARTESIAN PROCESSING ");
        memcpy(temp_single_led_pos_cartesian_update_msg.buf,serial_buf_copy,sizeof(temp_update_msg.buf));    // extra copy fuck it - it works
        // do update stuff
        // config coordinates are stored as cylindrical - need to convert first
        printf("convert coordinates to cylindrical ");
        temp_single_led_pos_cylindrical_update_msg.msg.s_msg.rgb_update = transform_cartesian_to_cylindrical(temp_single_led_pos_cartesian_update_msg.msg.s_msg.rgb_update);
        temp_single_led_pos_cylindrical_update_msg.msg.s_msg.led_string_position = temp_single_led_pos_cartesian_update_msg.msg.s_msg.led_string_position;
        printf("attempt write_flash_pos_config ");
        write_flash_pos_config(*reinterpret_cast<string_led_config*>(&temp_single_led_pos_cylindrical_update_msg.msg.s_msg));
        // Read back and print the actual stored result, independent of
        // write_flash_pos_config()'s own internal verify check - lets a
        // caller (or a human on a serial monitor) confirm the write really
        // took by eye, not just trust a pass/fail flag.
        print_pos_config(temp_single_led_pos_cylindrical_update_msg.msg.s_msg.led_string_position);
        new_msg = serial_msg_type::NOOP;
        msg_process_counter++;
        break;
    case serial_msg_type::LED_POS_UPDATE_CYLINDRICAL:
        printf("LED_POS_UPDATE_CYLINDRICAL PROCESSING ");
        memcpy(temp_single_led_pos_cylindrical_update_msg.buf,serial_buf_copy,sizeof(temp_update_msg.buf));    // extra copy fuck it - it works
        // do update stuff
        printf("attempt write_flash_pos_config ");
        write_flash_pos_config(*reinterpret_cast<string_led_config*>(&temp_single_led_pos_cylindrical_update_msg.msg.s_msg));
        print_pos_config(temp_single_led_pos_cylindrical_update_msg.msg.s_msg.led_string_position);
        new_msg = serial_msg_type::NOOP;
        msg_process_counter++;
        break;
    case serial_msg_type::READ_POS_CONFIG:
        memcpy(temp_read_pos_config_msg.buf,serial_buf_copy,sizeof(temp_update_msg.buf));    // extra copy fuck it - it works
        print_pos_config(temp_read_pos_config_msg.msg.s_msg.led_string_position);
        new_msg = serial_msg_type::NOOP;
        msg_process_counter++;
        break;
    case serial_msg_type::SET_VOLUME_CARTESIAN:
        memcpy(temp_set_volume_cartesian_msg.buf,serial_buf_copy,sizeof(temp_update_msg.buf));    // extra copy fuck it - it works
        engine_host_canvas_volume_cartesian(&(temp_set_volume_cartesian_msg.msg.s_msg));
        new_msg = serial_msg_type::NOOP;
        msg_process_counter++;
        break;
    case serial_msg_type::SET_VOLUME_CYLINDRICAL:
        memcpy(temp_set_volume_cylindrical_msg.buf,serial_buf_copy,sizeof(temp_update_msg.buf));    // extra copy fuck it - it works
        engine_host_canvas_volume_cylindrical(&(temp_set_volume_cylindrical_msg.msg.s_msg));
        new_msg = serial_msg_type::NOOP;
        msg_process_counter++;
        break;
    case serial_msg_type::CONFIG_RELOAD:
        memcpy(temp_config_reload_msg.buf,serial_buf_copy,sizeof(temp_update_msg.buf));    // extra copy fuck it - it works
        // do update stuff
        RGB_LED_3D::initialize_from_config();
        engine_host_reload_geometry();
        new_msg = serial_msg_type::NOOP;
        msg_process_counter++;
        break;
    case serial_msg_type::RESET_POS_CONFIG_TO_DEFAULT:
        reset_pos_config_to_default();
        RGB_LED_3D::initialize_from_config();
        engine_host_reload_geometry();
        new_msg = serial_msg_type::NOOP;
        msg_process_counter++;
        break;
    case serial_msg_type::ALL_LED_UPDATE_BASE:
        memcpy(temp_all_led_update_msg.buf,serial_buf_copy,sizeof(temp_update_msg.buf));    // extra copy fuck it - it works
        engine_host_canvas_base(&(temp_all_led_update_msg.msg.s_msg));
        new_msg = serial_msg_type::NOOP;
        msg_process_counter++;
        break;
    case serial_msg_type::WIFI_CRED_CHUNK:
        wifi_handle_cred_chunk(&reinterpret_cast<const wifi_cred_chunk_frame *>(serial_buf_copy)->s_msg);
        // Scrub the password bytes out of the shared receive buffer.
        memset(serial_buf_copy, 0, sizeof(serial_buf_copy));
        new_msg = serial_msg_type::NOOP;
        msg_process_counter++;
        break;
    case serial_msg_type::WIFI_CRED_COMMIT:
        wifi_handle_cred_commit(&reinterpret_cast<const wifi_cred_commit_frame *>(serial_buf_copy)->s_msg);
        new_msg = serial_msg_type::NOOP;
        msg_process_counter++;
        break;
    case serial_msg_type::TREE_OUTPUT:
        tree_output_enabled = (serial_buf_copy[1] != 0);
        engine_host_set_output(tree_output_enabled);
        event_logf("tree output %s", tree_output_enabled ? "ON" : "OFF");
        new_msg = serial_msg_type::NOOP;
        msg_process_counter++;
        break;
    case serial_msg_type::LED_OUTPUT_MODE:
        if (serial_buf_copy[1] < led_output_mode_count)
        {
            led_output_set_mode(static_cast<led_output_mode>(serial_buf_copy[1]));
            event_logf("led output mode %u", serial_buf_copy[1]);
        }
        else
        {
            printf("led output mode %u invalid\n", serial_buf_copy[1]);
        }
        new_msg = serial_msg_type::NOOP;
        msg_process_counter++;
        break;
    case serial_msg_type::LED_OUTPUT_TUNING:
    {
        led_output_tuning_t t;
        t.fast_slew = serial_buf_copy[1] != 0;
        t.drive_strength = serial_buf_copy[2] & 0x3;
        t.stagger_ns = serial_buf_copy[3] * 10u;
        t.phase_map = serial_buf_copy[5];
        led_output_set_tuning(t);
        if (serial_buf_copy[4] != 0)
        {
            target_loop_rate = serial_buf_copy[4];
        }
        static const unsigned drive_ma[4] = {2, 4, 8, 12};
        event_logf("output tuning: %s slew %umA stagger %uns %ufps phases %u%u%u%u",
                   t.fast_slew ? "fast" : "slow", drive_ma[t.drive_strength], (unsigned)t.stagger_ns,
                   (unsigned)target_loop_rate, t.phase_map & 3u, (t.phase_map >> 2) & 3u, (t.phase_map >> 4) & 3u,
                   (t.phase_map >> 6) & 3u);
        new_msg = serial_msg_type::NOOP;
        msg_process_counter++;
        break;
    }
    case serial_msg_type::STATUS_REQUEST:
    {
        // Over the network this is answered by the server on core1; this is
        // the USB serial path.
        static char status[status_json_max];   // static: keep it off core0's stack
        status_build_json(status, sizeof(status));
        printf("status: %s\n", status);
        new_msg = serial_msg_type::NOOP;
        msg_process_counter++;
        break;
    }
    case serial_msg_type::REBOOT:
        // The network ACK went out when the command was queued; the delay
        // gives lwIP time to actually transmit it.
        event_logf("reboot requested - restarting in 250ms");
        safety_stop_feeding();   // or the main loop would keep deferring it
        watchdog_reboot(0, 0, 250);
        new_msg = serial_msg_type::NOOP;
        msg_process_counter++;
        break;
    case serial_msg_type::DESCRIBE:
    {
        // Over the network the server answers this on core1.
        static char modes_json[status_json_max];   // static: keep it off core0's stack
        neotree::describe_modes(modes_json, sizeof(modes_json));
        printf("describe: %s\n", modes_json);
        new_msg = serial_msg_type::NOOP;
        msg_process_counter++;
        break;
    }
    case serial_msg_type::SLOT_SET:
    case serial_msg_type::PARAM_SET:
    case serial_msg_type::SLOT_END:
    case serial_msg_type::SLOT_LIFE:
    case serial_msg_type::INPUT:
    case serial_msg_type::PRESET:
        if (!engine_host_mode_command(serial_buf_copy, engine_mode_command_max_len))
        {
            event_logf("mode command %u rejected", (unsigned)serial_buf_copy[0]);
        }
        new_msg = serial_msg_type::NOOP;
        msg_process_counter++;
        break;
    case serial_msg_type::DEMO:
        if (!engine_host_set_demo(serial_buf_copy[1]))
        {
            event_logf("demo: unknown id %u", (unsigned)serial_buf_copy[1]);
        }
        new_msg = serial_msg_type::NOOP;
        msg_process_counter++;
        break;
    case serial_msg_type::BOOTSEL:
        event_logf("BOOTSEL requested - entering the USB flasher in 250ms");
        sleep_ms(250);   // lets the network ACK go out
        safety_reboot_to_bootsel();
    case serial_msg_type::WIFI_RECONNECT:
        wifi_request_reconnect();
        new_msg = serial_msg_type::NOOP;
        msg_process_counter++;
        break;

    default:
        // not a valid msg_type
        printf("Not a valid msg type");
        break;
    }
    
}

// Result of cyw43_arch_init() on core1 (0 = OK). Starts at a sentinel so the
// heartbeat distinguishes "init never returned" from a real error code.
#define CYW43_INIT_PENDING 1
volatile int cyw43_init_result = CYW43_INIT_PENDING;

void main_core1()
{
    // code for second core
    // Register this core as a lockout "victim" so core0 can pause it via
    // multicore_lockout_start/end_blocking() during flash erase/program in
    // write_flash_pos_config() (neo_tree_config.cpp). This core spins in
    // the serial-read loop below continuously, executing from flash (XIP)
    // the whole time - flash erase/program is documented as unsafe unless
    // the other core is prevented from fetching from flash concurrently.
    multicore_lockout_victim_init();
    // cyw43_arch_init() used to hang here intermittently. Likely root cause:
    // the CYW43 driver talks to the WiFi chip over a PIO state machine it
    // gets via pio_claim_free_sm_and_add_program_for_gpio_range() - i.e. the
    // first *unclaimed* SM, pio0 first. The WS2812 init in main() used to
    // run concurrently with this on core0 and program pio0 SMs 0-3 without
    // claiming them, so it could silently overwrite the CYW43's SPI state
    // machine mid-init. main() now claims those SMs and finishes PIO setup
    // before launching this core, so the driver lands on a free SM.
    // Initialised here (not on core0) so the CYW43 IRQ handlers run on this
    // core - core0 masks interrupts for ~9ms per string in write_string().
    cyw43_init_result = cyw43_arch_init();
    bool wifi_ok = (cyw43_init_result == 0);
    // Non-blocking: the connect proceeds in the background and wifi_poll()
    // below handles retries, so serial reading starts immediately.
    if (wifi_ok)
    {
        wifi_start();
        net_server_start();
    }
    // Watchdog heartbeat for core1 itself, mirroring the core0 one, so we
    // can see whether this loop is actually cycling (and how fast) once
    // past init, independent of whether any serial data ever arrives.
    uint64_t last_core1_heartbeat_us = 0;
    const uint64_t core1_heartbeat_interval_us = 5'000'000;
    uint64_t core1_loop_count = 0;
    uint64_t last_led_toggle_us = 0;
    const uint64_t led_toggle_interval_us = 500'000;
    bool led_on = false;
    while (true) {
        core1_loop_count++;
        core1_loop_counter = core1_loop_counter + 1;
        uint64_t now_us = time_us_64();
        if (now_us > (last_core1_heartbeat_us + core1_heartbeat_interval_us))
        {
            last_core1_heartbeat_us = now_us;
            printf("core1 alive: loop_count=%u cmd_queue=%u dropped=%u net_clients=%u net_cmds=%u "
                   "cyw43_init=%d wifi: %s\n",
                   (uint32_t)core1_loop_count, (unsigned)command_queue_level(),
                   (unsigned)command_queue_dropped(), (unsigned)net_server_client_count(),
                   (unsigned)net_server_commands_received(), (int)cyw43_init_result,
                   wifi_ok ? wifi_status_str() : "n/a");
            net_server_diag_t nd = net_server_diag();
            printf("net diag: accepts=%u accept_errors=%u writes_deferred=%u write_failures=%u (last err %d) "
                   "output_failures=%u overflow_closes=%u\n",
                   (unsigned)nd.accepts, (unsigned)nd.accept_errors, (unsigned)nd.writes_deferred,
                   (unsigned)nd.write_failures, (int)nd.last_write_error, (unsigned)nd.output_failures,
                   (unsigned)nd.overflow_closes);
            net_server_print_lwip_stats();
            wifi_print_trace();
        }
        if (wifi_ok)
        {
            wifi_poll();
            net_server_poll();
        }
        // Onboard LED blinks at 1Hz while core1 is cycling and the CYW43 is
        // up. The LED hangs off the CYW43, so each write is an SPI
        // transaction to the WiFi chip - toggling it every loop (as this
        // used to) throttled the serial-read loop to a few hundred Hz.
        if (wifi_ok && now_us > (last_led_toggle_us + led_toggle_interval_us))
        {
            last_led_toggle_us = now_us;
            led_on = !led_on;
            cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, led_on);
        }
        serial_read_buffer();
    }
}

bool frame_prepared = false;
uint64_t next_frame_us = 0;
volatile uint64_t initial_abs_time_check = 0;
uint64_t initial_startup_delay = 1'500'00;
volatile uint64_t latest_abs_time_check = 0;
uint64_t led_loop_counter = 0;
// Drain everything queued each pass: handlers take microseconds, and LED
// output runs on DMA, so there's no reason to leave commands waiting.
const int max_commands_per_loop = command_queue_capacity;

// Watchdog heartbeat - prints uptime over serial every 5s regardless of
// whether any command has been received, so liveness of the main core0
// loop can be confirmed independent of the serial link.
volatile uint64_t last_uptime_print_us = 0;
const uint64_t uptime_print_interval_us = 5'000'000;   // 5 seconds

int main() {
    // Before anything that could crash: crash-loop escape + watchdog.
    safety_boot();
    safety_paint_stacks();
    //set_sys_clock_48();
    stdio_init_all();

    // Deploy-pipeline proof marker - a fresh, one-off token picked at the
    // time this change was written, not derived from anything already on
    // the board, so seeing it over serial after a deploy proves this exact
    // build is what's actually running.
    printf("\nDEPLOY MARKER: 081f4c04\n");

    safety_report_t safety = safety_report();
    status_boot_was_watchdog = safety.crash_reboot;
    event_log_init();
    status_init();
    event_logf("boot (%s)", status_boot_was_watchdog ? "watchdog reboot" : "power-on or reset");
    if (safety.fault_core >= 0)
    {
        event_logf("crash #%u: core%d fault pc 0x%08lx lr 0x%08lx", (unsigned)safety.crash_count,
                   (int)safety.fault_core, (unsigned long)safety.fault_pc, (unsigned long)safety.fault_lr);
    }
    else if (safety.crash_reboot)
    {
        event_logf("crash #%u: a core stopped (hang, no fault)", (unsigned)safety.crash_count);
    }

    // Claims its PIO SMs and DMA channels - finish before core1 starts the
    // CYW43 driver, whose own PIO/DMA allocation must land elsewhere.
    led_output_init();

    command_queue_init();   // core1's serial reader pushes into it from the start
    multicore_launch_core1(main_core1);

    init_my_tree();
    watchdog_update();
    // Load any persisted LED position config from flash (falls back to
    // compiled-in defaults if flash doesn't hold a valid config yet) and
    // apply it to the tree. Previously this only happened on an explicit
    // CONFIG_RELOAD serial command, so even a correctly-written config
    // would have had no effect after a real power cycle - the write path
    // existed but nothing ever loaded it back.
    load_pos_config_from_flash();
    RGB_LED_3D::initialize_from_config();
    watchdog_update();
    engine_host_init();
    watchdog_update();
    // grab first loop a abs time
    initial_abs_time_check = get_absolute_time();
    // adding a wait loop before starting the main while loop
    // this was necessary to stop the neopixel data from glitching out from something fucking up at startup
    // not a root cause solution, band-aid and lgtm
    while(get_absolute_time() < (initial_abs_time_check + initial_startup_delay)){;}
    //redo initial time check for loop timer
    initial_abs_time_check = get_absolute_time();
    next_frame_us = initial_abs_time_check;
    loop_monitor_init();
    loop_monitor_wrap_core0_irqs();
    while(1)
    {
        latest_abs_time_check = get_absolute_time();
        loop_monitor_pass_begin(latest_abs_time_check);
        safety_poll(latest_abs_time_check, core1_loop_counter);
        if (latest_abs_time_check > (last_uptime_print_us + uptime_print_interval_us))
        {
            last_uptime_print_us = latest_abs_time_check;
            led_output_stats_t out = led_output_take_stats();
            printf("uptime s: %u marker: 081f4c04 led_loop_counter: %u target_fps: %u output_mode: %u "
                   "prepare_us: %u (max %u) output_us: %u (max %u)\n",
                   (uint32_t)(latest_abs_time_check / 1'000'000), (uint32_t)led_loop_counter,
                   (unsigned)target_loop_rate, (unsigned)led_output_get_mode(),
                   (unsigned)out.last_prepare_us, (unsigned)out.max_prepare_us,
                   (unsigned)out.last_output_us, (unsigned)out.max_output_us);
            printf("led output faults per string - underflow: %u %u %u %u overflow: %u %u %u %u\n",
                   (unsigned)out.underflows[0], (unsigned)out.underflows[1], (unsigned)out.underflows[2],
                   (unsigned)out.underflows[3], (unsigned)out.overflows[0], (unsigned)out.overflows[1],
                   (unsigned)out.overflows[2], (unsigned)out.overflows[3]);
            loop_monitor_mark(loop_pass_heartbeat);
        }
        // Bounded per pass so a burst of commands can't starve the LED refresh.
        static command_t cmd;   // static: ~260 bytes, keep it off core0's stack
        for (int i = 0; i < max_commands_per_loop && command_queue_pop(&cmd); i++)
        {
            memcpy(serial_buf_copy, cmd.data, SERIAL_BUFFER_SIZE);
            memset(cmd.data, 0, sizeof(cmd.data));  // may hold WiFi credential bytes
            process_msg();
            loop_monitor_mark(loop_pass_commands);
        }
        // Frame pacing: when the next frame is due, pack it (cheap), then start
        // it as soon as the previous one has fully gone out and latched. The
        // output itself runs on DMA, so this loop never blocks on it.
        if (!frame_prepared && latest_abs_time_check >= next_frame_us)
        {
            static uint32_t frame_words[max_led_config_size];   // static: 4 KB
            engine_host_frame(latest_abs_time_check, frame_words, max_led_config_size);
            led_output_prepare_frame(frame_words);
            frame_prepared = true;
            loop_monitor_mark(loop_pass_frame_prep);
        }
        // Polled every pass (not just when a frame is waiting) so the end of
        // each frame is timestamped promptly for the output stats.
        bool output_ready = led_output_ready();
        if (frame_prepared && output_ready)
        {
            led_output_start_frame();
            frame_prepared = false;
            loop_monitor_mark(loop_pass_frame_start);
            led_loop_counter++;
            // Schedule from the ideal time so the rate doesn't drift, but never
            // "catch up" with a burst after a stall (or at a rate the output
            // can't reach) - just run frames back to back.
            next_frame_us += 1'000'000 / target_loop_rate;
            if (next_frame_us < latest_abs_time_check)
            {
                next_frame_us = latest_abs_time_check;
            }
        }
    }

}
