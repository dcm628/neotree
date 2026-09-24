#include "neo_tree_led_output.hpp"

#include "hardware/dma.h"
#include "hardware/pio.h"
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
static uint32_t dma_start_mask = 0;

// Double buffer: one being sent by DMA, the other being filled. Each word is
// a GRB color already shifted into the top 24 bits, as the PIO program
// shifts out MSB-first with autopull at 24 bits.
static uint32_t frame_buffers[2][total_leds];
static uint back_buffer = 0;

static bool frame_in_flight = false;
static bool stall_flags_armed = false;
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
        dma_start_mask |= 1u << ch;
    }
    latch_started_us = time_us_64();
}

void led_output_prepare_frame()
{
    uint64_t t0 = time_us_64();
    uint32_t *buf = frame_buffers[back_buffer];
    // Lights off (TREE_OUTPUT) still sends a full frame, just zeros - the
    // LEDs keep whatever they were last sent, so they have to be actively
    // written dark, and keep being written so they stay dark.
    if (tree_output_enabled)
    {
        for (uint i = 0; i < total_leds; i++)
        {
            buf[i] = RGB_LED_3D::string_vec[i]->get_grb_word() << 8u;
        }
    }
    else
    {
        for (uint i = 0; i < total_leds; i++)
        {
            buf[i] = 0;
        }
    }
    stats.last_prepare_us = (uint32_t)(time_us_64() - t0);
    if (stats.last_prepare_us > stats.max_prepare_us)
    {
        stats.max_prepare_us = stats.last_prepare_us;
    }
}

bool led_output_ready()
{
    if (frame_in_flight)
    {
        // DMA finishing only means the last words reached the FIFOs. The
        // frame is out once every FIFO is empty and every SM has stalled
        // trying to pull more - it stalls on "out x, 1 side 0", so the data
        // line is already held low from that moment.
        if (!stall_flags_armed)
        {
            for (uint s = 0; s < led_output_num_strings; s++)
            {
                if (dma_channel_is_busy(dma_channels[s]))
                {
                    return false;
                }
            }
            // Clear the (write-1-to-clear) TX stall flags only now: the SMs
            // were stalled when the frame started, so a flag cleared then
            // could re-latch before the first word arrived and read as
            // "done" while the last LED was still shifting out. Any stall
            // flagged from here on happened after the final word.
            pio->fdebug = 0b1111u << PIO_FDEBUG_TXSTALL_LSB;
            stall_flags_armed = true;
        }
        for (uint s = 0; s < led_output_num_strings; s++)
        {
            if (!pio_sm_is_tx_fifo_empty(pio, s) || !(pio->fdebug & (1u << (PIO_FDEBUG_TXSTALL_LSB + s))))
            {
                return false;
            }
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

void led_output_start_frame()
{
    const uint32_t *buf = frame_buffers[back_buffer];
    uint first = 0;
    for (uint s = 0; s < led_output_num_strings; s++)
    {
        dma_channel_set_read_addr(dma_channels[s], buf + first, false);
        dma_channel_set_trans_count(dma_channels[s], string_lengths[s], false);
        first += string_lengths[s];
    }
    frame_started_us = time_us_64();
    // All 4 strings start together.
    dma_start_channel_mask(dma_start_mask);
    frame_in_flight = true;
    stall_flags_armed = false;
    back_buffer ^= 1;
    stats.frames++;
}

led_output_stats_t led_output_take_stats()
{
    led_output_stats_t out = stats;
    stats.max_prepare_us = 0;
    stats.max_output_us = 0;
    return out;
}
