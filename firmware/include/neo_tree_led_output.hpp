#ifndef _NEO_TREE_LED_OUTPUT_HPP
#define _NEO_TREE_LED_OUTPUT_HPP

#include "pico/stdlib.h"

// Drives the 4 WS2812 strings with one PIO state machine and one DMA channel
// each, so a frame goes out on all strings in parallel without the CPU: it
// only packs the frame into a buffer (tens of microseconds) and starts the
// DMA. A frame then takes as long as the longest string (300 LEDs x 30us =
// 9ms) plus the latch gap, with interrupts left enabled throughout.
//
// Frame sequence on core0:
//   led_output_prepare_frame();            // any time - fills the back buffer
//   if (led_output_ready()) led_output_start_frame();

// LEDs per string, in string_vec order: string 0 is LEDs 0-299 (GP2), string 1
// 300-599 (GP5), string 2 600-799 (GP6), string 3 800-999 (GP7).
const uint led_output_num_strings = 4;

// core0, before launching core1: claims pio0 SMs 0-3 and 4 DMA channels -
// both properly claimed so the CYW43 driver's own PIO/DMA allocations can't
// collide with them (see main_core1()).
void led_output_init();

// Packs every LED's current color (or zeros while the lights are off) into
// the back buffer. Safe while the previous frame is still going out - that
// one is in the other buffer.
void led_output_prepare_frame();

// True once the previous frame has fully left every state machine (DMA done,
// FIFOs drained, line held low) and the WS2812 latch gap has passed.
bool led_output_ready();

// Starts DMA on all 4 strings from the prepared buffer and swaps buffers.
// Only call when led_output_ready().
void led_output_start_frame();

struct led_output_stats_t
{
    uint32_t frames;
    uint32_t last_prepare_us;
    uint32_t max_prepare_us;
    uint32_t last_output_us;   // start of DMA -> last bit out, as seen by led_output_ready()
    uint32_t max_output_us;
};
// Returns the stats and resets the maxima, so each call reports the worst
// case since the previous one (the heartbeat calls it every 5s).
led_output_stats_t led_output_take_stats();

#endif
