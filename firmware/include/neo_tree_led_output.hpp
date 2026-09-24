#ifndef _NEO_TREE_LED_OUTPUT_HPP
#define _NEO_TREE_LED_OUTPUT_HPP

#include "pico/stdlib.h"

// Drives the 4 WS2812 strings with one PIO state machine and one DMA channel
// each, so frames go out without the CPU: it only packs the frame into a
// buffer (~145us) and starts the DMA, with interrupts left enabled.
//
// Strings are sent in phases: strings 0+1 together, then 2+3 (15ms per
// frame, ~65 fps max; 60 fps target). All 4 in parallel (9ms) glitched on
// the real tree - see docs/LED_OUTPUT.md before changing the grouping.
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

// How a frame is sent. PARALLEL_DMA is the normal mode; the others send the
// same packed frame one string at a time (blocking) and exist to diagnose
// output glitches on the real tree - switchable live with LED_OUTPUT_MODE.
enum class led_output_mode : uint8_t
{
    PARALLEL_DMA = 0,
    SEQUENTIAL_DMA = 1,
    CPU_SEQUENTIAL_MASKED = 2,   // the pre-DMA method: interrupts off per string
    CPU_SEQUENTIAL = 3,
};
const uint8_t led_output_mode_count = 4;
void led_output_set_mode(led_output_mode mode);
led_output_mode led_output_get_mode();

// Diagnostic knobs for the data lines, switchable live with LED_OUTPUT_TUNING:
// GPIO slew rate and drive strength on the 4 data pins, and (PARALLEL_DMA
// only) a start offset between consecutive strings so their edges don't
// coincide. Boots with SDK defaults (slow slew, 4mA) and no stagger.
struct led_output_tuning_t
{
    bool fast_slew;
    uint8_t drive_strength;   // 0-3 = 2/4/8/12 mA
    uint32_t stagger_ns;      // between consecutive string starts; bit period is 1250ns
    // 2 bits per string (string s at bits 2s..2s+1): the phase it's sent in.
    // Strings in the same phase go out together; phases run in order, each
    // starting once the previous has drained. 0 = all parallel (0b00000000),
    // 0b11100100 = strings 0,1,2,3 one after another.
    uint8_t phase_map;
};
void led_output_set_tuning(const led_output_tuning_t &t);
led_output_tuning_t led_output_get_tuning();

struct led_output_stats_t
{
    uint32_t frames;
    uint32_t last_prepare_us;
    uint32_t max_prepare_us;
    uint32_t last_output_us;   // start of DMA -> last bit out, as seen by led_output_ready()
    uint32_t max_output_us;
    // PARALLEL_DMA only, per string: FIFO ran dry mid-string (a data gap) /
    // DMA wrote into a full FIFO (a dropped word).
    uint32_t underflows[led_output_num_strings];
    uint32_t overflows[led_output_num_strings];
};
// Returns the stats and resets the maxima, so each call reports the worst
// case since the previous one (the heartbeat calls it every 5s).
led_output_stats_t led_output_take_stats();

#endif
