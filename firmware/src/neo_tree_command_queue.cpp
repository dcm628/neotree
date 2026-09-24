#include "neo_tree_command_queue.hpp"

#include <cstring>

#include "pico/util/queue.h"

static const uint command_queue_depth = command_queue_capacity;

static queue_t command_queue;
// Diagnostic only - increments from different contexts aren't atomic, so it
// can under-count under contention.
static volatile uint32_t dropped_count = 0;

void command_queue_init()
{
    queue_init(&command_queue, sizeof(command_t), command_queue_depth);
}

bool command_queue_push(command_source source, uint8_t client_id, const uint8_t *data, size_t len)
{
    if (len == 0 || len > command_max_len)
    {
        dropped_count = dropped_count + 1;
        return false;
    }
    // Built on the caller's stack, not in a shared static: pushes come from
    // core1's loop and from lwIP IRQs on the same core, which can interleave.
    // ~260 bytes, well within core1's 4KB stack.
    command_t cmd;
    cmd.source = source;
    cmd.client_id = client_id;
    cmd.len = static_cast<uint16_t>(len);
    memcpy(cmd.data, data, len);
    // Handlers read fixed-size structs out of the buffer, so bytes past len
    // must be zero (matching what the serial path always provided).
    memset(cmd.data + len, 0, command_max_len - len);
    if (!queue_try_add(&command_queue, &cmd))
    {
        dropped_count = dropped_count + 1;
        return false;
    }
    return true;
}

bool command_queue_pop(command_t *out)
{
    return queue_try_remove(&command_queue, out);
}

uint32_t command_queue_level()
{
    return queue_get_level(&command_queue);
}

uint32_t command_queue_dropped()
{
    return dropped_count;
}
