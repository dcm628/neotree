#include "neo_tree_led_output.hpp"

#include <cstring>

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/sync.h"
#include "ws2812.pio.h"

#include "dcm_rgb.hpp"
#include "neo_tree_protocol.hpp"

static const uint string_pins[led_output_num_strings] = {2, 5, 6, 7};
static const uint string_lengths[led_output_num_strings] = {300, 300, 200, 200};
static const uint total_leds = 1000;
static_assert(300 + 300 + 200 + 200 == total_leds, "string lengths must cover every LED");

// WS2812s latch a frame once the data line has been low this long. 50us is
// the original WS2812 spec; newer WS2812B parts need 280us - use a margin
// over the larger.
static const uint64_t latch_us = 300;

static PIO const pio = pio0;
static int dma_channels[led_output_num_strings];

// Double buffer: one being sent by DMA, the other being filled. Each word is
// a GRB color already shifted into the top 24 bits, as the PIO program
// shifts out MSB-first with autopull at 24 bits.
static uint32_t frame_buffers[2][total_leds];
static uint back_buffer = 0;

static led_output_mode output_mode = led_output_mode::PARALLEL_DMA;
// SDK GPIO defaults: slow slew, 4mA drive. No stagger. Strings 0+1 then 2+3
// (see string_phase).
static led_output_tuning_t tuning = {false, GPIO_DRIVE_STRENGTH_4MA, 0, 0b01010000};
static uint32_t stagger_cycles = 0;
static bool frame_in_flight = false;
static bool stall_flags_armed = false;
static bool midframe_armed = false;
// Phase of each string (0-3); strings sharing a phase are sent together.
// Default: strings 0+1 (GP2, GP5) together, then strings 2+3 (GP6, GP7) -
// 15ms per frame. With all 4 in parallel (9ms) strings 2 and 3 glitched
// visibly on the tree (2026-09-24), although every SM's FIFO stayed fed
// (zero underflows/overflows) and each pin's bitstream was identical to the
// clean sequential modes; offsetting their starts by 300ns only reduced it.
// These two pairs are clean together with no offset.
static uint8_t string_phase[led_output_num_strings] = {0, 0, 1, 1};
static uint current_phase = 0;
static uint32_t phase_strings = 0;   // bit s set = string s is in the current phase
static uint64_t phase_started_us = 0;
static uint64_t frame_started_us = 0;
static uint64_t latch_started_us = 0;
static led_output_stats_t stats = {};

void led_output_init()
{
    pio_claim_sm_mask(pio, 0b1111);
    uint offset = pio_add_program(pio, &ws2812_program);
    for (uint s = 0; s < led_output_num_strings; s++)
    {
        ws2812_program_init(pio, s, offset, string_pins[s], 800000, false);

        int ch = dma_claim_unused_channel(true);
        dma_channel_config c = dma_channel_get_default_config(ch);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
        channel_config_set_read_increment(&c, true);
        channel_config_set_write_increment(&c, false);
        // Paced by the SM's TX FIFO: DMA only writes when there's room, so it
        // feeds the string at exactly the rate the PIO clocks bits out.
        channel_config_set_dreq(&c, pio_get_dreq(pio, s, true));
        dma_channel_configure(ch, &c, &pio->txf[s], nullptr, 0, false);
        dma_channels[s] = ch;
    }
    latch_started_us = time_us_64();
}

void led_output_prepare_frame(const uint32_t *words)
{
    uint64_t t0 = time_us_64();
    // Lights off is rendered upstream as an all-zero frame: the LEDs keep
    // whatever they were last sent, so they have to be actively written dark
    // every frame.
    memcpy(frame_buffers[back_buffer], words, sizeof(frame_buffers[back_buffer]));
    stats.last_prepare_us = (uint32_t)(time_us_64() - t0);
    if (stats.last_prepare_us > stats.max_prepare_us)
    {
        stats.max_prepare_us = stats.last_prepare_us;
    }
}

// Masks of fdebug bits for the strings in `strings` (bit s = string s).
static uint32_t txstall_bits(uint32_t strings) { return strings << PIO_FDEBUG_TXSTALL_LSB; }
static uint32_t txover_bits(uint32_t strings) { return strings << PIO_FDEBUG_TXOVER_LSB; }

// Starts the DMA for every string in the current phase (addresses and counts
// were set at frame start), `stagger_cycles` apart so their edges don't
// coincide.
static void start_phase()
{
    phase_strings = 0;
    for (uint s = 0; s < led_output_num_strings; s++)
    {
        if (string_phase[s] == current_phase)
        {
            phase_strings |= 1u << s;
        }
    }
    uint32_t irq = save_and_disable_interrupts();   // a few us at most, keeps the offsets exact
    bool first = true;
    for (uint s = 0; s < led_output_num_strings; s++)
    {
        if (!(phase_strings & (1u << s)))
        {
            continue;
        }
        if (!first && stagger_cycles)
        {
            busy_wait_at_least_cycles(stagger_cycles);
        }
        dma_channel_start(dma_channels[s]);
        first = false;
    }
    restore_interrupts(irq);
    phase_started_us = time_us_64();
    stall_flags_armed = false;
    midframe_armed = false;
}

// Next phase that has at least one string, or led_output_num_strings if none.
static uint next_phase_after(uint phase)
{
    for (uint p = phase + 1; p < led_output_num_strings; p++)
    {
        for (uint s = 0; s < led_output_num_strings; s++)
        {
            if (string_phase[s] == p)
            {
                return p;
            }
        }
    }
    return led_output_num_strings;
}

bool led_output_ready()
{
    if (frame_in_flight)
    {
        // Diagnostics. A TX stall on an SM whose DMA is still busy means its
        // FIFO ran dry mid-string - a gap in the data, which the LEDs take as
        // end-of-frame (a segmented stream). TXOVER means DMA wrote into a
        // full FIFO and a word was dropped. Stall checking starts 50us into
        // the phase, once DMA has primed the FIFOs (the SMs were
        // legitimately stalled before it started). fdebug is read before the
        // busy flags, so "busy now" implies busy when the stall was flagged.
        uint32_t fd = pio->fdebug;
        if (!midframe_armed)
        {
            if (time_us_64() - phase_started_us >= 50)
            {
                pio->fdebug = txstall_bits(phase_strings);
                midframe_armed = true;
            }
        }
        else if (!stall_flags_armed)
        {
            uint32_t clear = 0;
            for (uint s = 0; s < led_output_num_strings; s++)
            {
                uint32_t bit = txstall_bits(1u << s);
                if ((phase_strings & (1u << s)) && (fd & bit) && dma_channel_is_busy(dma_channels[s]))
                {
                    stats.underflows[s]++;
                    stats.underflows_total[s]++;
                    clear |= bit;
                }
            }
            if (clear)
            {
                pio->fdebug = clear;
            }
        }
        if (fd & txover_bits(0b1111u))
        {
            for (uint s = 0; s < led_output_num_strings; s++)
            {
                if (fd & txover_bits(1u << s))
                {
                    stats.overflows[s]++;
                    stats.overflows_total[s]++;
                }
            }
            pio->fdebug = fd & txover_bits(0b1111u);
        }

        // DMA finishing only means the last words reached the FIFOs. A phase
        // is out once each of its FIFOs is empty and each SM has stalled
        // trying to pull more - it stalls on "out x, 1 side 0", so the data
        // line is already held low from that moment.
        if (!stall_flags_armed)
        {
            for (uint s = 0; s < led_output_num_strings; s++)
            {
                if ((phase_strings & (1u << s)) && dma_channel_is_busy(dma_channels[s]))
                {
                    return false;
                }
            }
            // Clear the (write-1-to-clear) TX stall flags only now: the SMs
            // were stalled when the phase started, so a flag cleared then
            // could re-latch before the first word arrived and read as
            // "done" while the last LED was still shifting out. Any stall
            // flagged from here on happened after the final word.
            pio->fdebug = txstall_bits(phase_strings);
            stall_flags_armed = true;
        }
        for (uint s = 0; s < led_output_num_strings; s++)
        {
            if ((phase_strings & (1u << s)) &&
                (!pio_sm_is_tx_fifo_empty(pio, s) || !(pio->fdebug & txstall_bits(1u << s))))
            {
                return false;
            }
        }
        // This phase's strings are out - start the next phase, if any. Other
        // strings are unaffected, so no latch gap is needed between phases.
        uint next = next_phase_after(current_phase);
        if (next < led_output_num_strings)
        {
            current_phase = next;
            start_phase();
            return false;
        }
        frame_in_flight = false;
        latch_started_us = time_us_64();
        stats.last_output_us = (uint32_t)(latch_started_us - frame_started_us);
        if (stats.last_output_us > stats.max_output_us)
        {
            stats.max_output_us = stats.last_output_us;
        }
    }
    return time_us_64() - latch_started_us >= latch_us;
}

// Blocks until SM s has shifted out everything it was given: FIFO empty and
// a TX stall flagged after the caller cleared the flags.
static void wait_string_drained(uint s)
{
    while (!pio_sm_is_tx_fifo_empty(pio, s) || !(pio->fdebug & (1u << (PIO_FDEBUG_TXSTALL_LSB + s))))
    {
        tight_loop_contents();
    }
}

// Diagnostic modes: send the same packed frame one string at a time,
// blocking, by DMA or by CPU writes. Used to isolate what makes the
// parallel mode glitch on the real tree.
static void send_frame_sequential(const uint32_t *buf)
{
    uint first = 0;
    for (uint s = 0; s < led_output_num_strings; s++)
    {
        if (output_mode == led_output_mode::SEQUENTIAL_DMA)
        {
            dma_channel_set_read_addr(dma_channels[s], buf + first, false);
            dma_channel_set_trans_count(dma_channels[s], string_lengths[s], true);
            dma_channel_wait_for_finish_blocking(dma_channels[s]);
        }
        else
        {
            uint32_t irq = 0;
            bool masked = (output_mode == led_output_mode::CPU_SEQUENTIAL_MASKED);
            if (masked)
            {
                irq = save_and_disable_interrupts();
            }
            for (uint i = 0; i < string_lengths[s]; i++)
            {
                pio_sm_put_blocking(pio, s, buf[first + i]);
            }
            if (masked)
            {
                restore_interrupts(irq);
            }
        }
        pio->fdebug = 1u << (PIO_FDEBUG_TXSTALL_LSB + s);
        wait_string_drained(s);
        first += string_lengths[s];
    }
}

void led_output_start_frame()
{
    const uint32_t *buf = frame_buffers[back_buffer];
    frame_started_us = time_us_64();
    back_buffer ^= 1;
    stats.frames++;
    if (output_mode != led_output_mode::PARALLEL_DMA)
    {
        send_frame_sequential(buf);
        latch_started_us = time_us_64();
        stats.last_output_us = (uint32_t)(latch_started_us - frame_started_us);
        if (stats.last_output_us > stats.max_output_us)
        {
            stats.max_output_us = stats.last_output_us;
        }
        return;
    }
    uint first = 0;
    for (uint s = 0; s < led_output_num_strings; s++)
    {
        dma_channel_set_read_addr(dma_channels[s], buf + first, false);
        dma_channel_set_trans_count(dma_channels[s], string_lengths[s], false);
        first += string_lengths[s];
    }
    // Strings are sent in phases (string_phase): all strings in a phase go
    // out together, and led_output_ready() starts the next phase once the
    // previous one has drained. All in phase 0 = fully parallel.
    current_phase = led_output_num_strings;
    for (uint s = 0; s < led_output_num_strings; s++)
    {
        if (string_phase[s] < current_phase)
        {
            current_phase = string_phase[s];
        }
    }
    frame_in_flight = true;
    start_phase();
}

void led_output_set_tuning(const led_output_tuning_t &t)
{
    tuning = t;
    stagger_cycles = (uint32_t)((uint64_t)t.stagger_ns * clock_get_hz(clk_sys) / 1'000'000'000u);
    // Takes effect from the next frame (start_frame reads string_phase).
    for (uint s = 0; s < led_output_num_strings; s++)
    {
        string_phase[s] = (t.phase_map >> (2 * s)) & 0x3;
    }
    for (uint s = 0; s < led_output_num_strings; s++)
    {
        gpio_set_slew_rate(string_pins[s], t.fast_slew ? GPIO_SLEW_RATE_FAST : GPIO_SLEW_RATE_SLOW);
        gpio_set_drive_strength(string_pins[s], static_cast<gpio_drive_strength>(t.drive_strength));
    }
}

led_output_tuning_t led_output_get_tuning()
{
    return tuning;
}

void led_output_set_mode(led_output_mode mode)
{
    output_mode = mode;
}

led_output_mode led_output_get_mode()
{
    return output_mode;
}

led_output_stats_t led_output_take_stats()
{
    led_output_stats_t out = stats;
    stats.max_prepare_us = 0;
    stats.max_output_us = 0;
    for (uint s = 0; s < led_output_num_strings; s++)
    {
        stats.underflows[s] = 0;
        stats.overflows[s] = 0;
    }
    return out;
}

led_output_stats_t led_output_peek_stats()
{
    return stats;
}
