#ifndef _NEO_TREE_COMMAND_QUEUE_HPP
#define _NEO_TREE_COMMAND_QUEUE_HPP

#include "pico/stdlib.h"

// Single path for every incoming command, whatever transport it arrived on.
// Producers (core1: the USB serial reader and the network server, including
// from lwIP callbacks in IRQ context) push complete messages; core0 pops and
// dispatches them through process_msg(). Backed by pico_util's queue_t,
// which is spinlock-protected and safe across cores and IRQs.

// Largest single message - the biggest message struct is well under this
// (see the *_frame structs in main.cpp).
const size_t command_max_len = 256;

// Queue depth (x ~260 bytes each). core0 only drains between LED frames
// (~30ms each while it pushes all 4 strings out), so this has to absorb a
// frame's worth of commands from a fast sender - the network server answers
// QUEUE_FULL beyond that and clients back off.
const size_t command_queue_capacity = 32;

enum class command_source : uint8_t
{
    usb_serial,
    network,
};

struct command_t
{
    command_source source;
    uint8_t client_id;  // network connection slot; 0 for usb_serial
    uint16_t len;
    uint8_t data[command_max_len];
};

// Call once on core0 before launching core1.
void command_queue_init();

// Copies the message in. Returns false (and counts a drop) if it's too long
// or the queue is full. Safe from either core and from IRQs.
bool command_queue_push(command_source source, uint8_t client_id, const uint8_t *data, size_t len);

// core0: pops the oldest message, if any.
bool command_queue_pop(command_t *out);

uint32_t command_queue_level();
uint32_t command_queue_dropped();

#endif
