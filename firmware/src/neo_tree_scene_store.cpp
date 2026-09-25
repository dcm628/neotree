#include "neo_tree_scene_store.hpp"

#include <cstring>

#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"   // XIP_BASE
#include "pico/stdlib.h"

#include "neo_tree_config.hpp"
#include "neo_tree_event_log.hpp"
#include "neotree/library.hpp"

namespace {

constexpr uint32_t store_magic = 0x4C53544E;   // "NTSL"
constexpr size_t store_size = 4 * FLASH_SECTOR_SIZE;

struct store_header
{
    uint32_t magic;
    uint32_t length;   // of the stored form that follows
    uint32_t crc;
    uint32_t reserved;
};
constexpr size_t payload_max = store_size - sizeof(store_header);

// Static: the stored form can be most of 16 KB - far too big for a stack.
uint8_t staging[store_size];
scene_store_stats_t stats = {};

// End of the program image in flash, from the linker script.
extern "C" char __flash_binary_end;

uint32_t store_offset()
{
    // Below the WiFi credentials sector, which is below the LED positions.
    return pos_config_flash_offset() - FLASH_SECTOR_SIZE - store_size;
}

bool store_clear_of_program()
{
    return XIP_BASE + store_offset() >= reinterpret_cast<uintptr_t>(&__flash_binary_end);
}

uint32_t crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (int k = 0; k < 8; k++)
        {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

}  // namespace

scene_store_load_result scene_store_load(neotree::Library &library)
{
    stats.load = scene_store_load_result::none;
    if (!store_clear_of_program())
    {
        event_logf("scene store: overlaps the program - not used");
        stats.load = scene_store_load_result::invalid;
        return stats.load;
    }
    const uint8_t *flash = reinterpret_cast<const uint8_t *>(XIP_BASE + store_offset());
    store_header h;
    memcpy(&h, flash, sizeof(h));
    if (h.magic != store_magic)
    {
        return stats.load;   // never written (erased flash reads 0xFF)
    }
    const uint8_t *payload = flash + sizeof(h);
    if (h.length > payload_max || crc32(payload, h.length) != h.crc || !library.load(payload, h.length))
    {
        event_logf("scene store: stored library is damaged - using the built-ins");
        stats.load = scene_store_load_result::invalid;
        return stats.load;
    }
    stats.load = scene_store_load_result::loaded;
    stats.stored_bytes = h.length;
    event_logf("scene store: library loaded (%u bytes)", (unsigned)h.length);
    return stats.load;
}

bool scene_store_save(const neotree::Library &library)
{
    const size_t n = store_clear_of_program() ? library.save(staging + sizeof(store_header), payload_max) : 0;
    if (n == 0)
    {
        stats.save_failures++;
        event_logf("scene store: library too big to store - not saved");
        return false;
    }
    store_header h = {store_magic, static_cast<uint32_t>(n), crc32(staging + sizeof(store_header), n), 0};
    memcpy(staging, &h, sizeof(h));
    // Only the sectors in use: a sector erase is the slow part.
    const size_t used = sizeof(h) + n;
    const size_t bytes = (used + FLASH_SECTOR_SIZE - 1) / FLASH_SECTOR_SIZE * FLASH_SECTOR_SIZE;
    memset(staging + used, 0xFF, bytes - used);
    const uint64_t t0 = time_us_64();
    flash_write_sectors_locked(store_offset(), staging, bytes);
    stats.last_save_us = static_cast<uint32_t>(time_us_64() - t0);

    const uint8_t *flash = reinterpret_cast<const uint8_t *>(XIP_BASE + store_offset());
    if (memcmp(flash, staging, used) != 0)
    {
        stats.save_failures++;
        event_logf("scene store: verify FAILED after writing %u bytes", (unsigned)n);
        return false;
    }
    stats.saves++;
    stats.stored_bytes = n;
    event_logf("scene store: library saved (%u bytes, %u ms)", (unsigned)n, (unsigned)(stats.last_save_us / 1000));
    return true;
}

scene_store_stats_t scene_store_stats()
{
    return stats;
}
