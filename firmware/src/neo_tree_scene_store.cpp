#include "neo_tree_scene_store.hpp"

#include <cstring>

#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"   // XIP_BASE
#include "pico/stdlib.h"

#include "neo_tree_config.hpp"
#include "neo_tree_event_log.hpp"
#include "neotree/library.hpp"

namespace {

constexpr uint32_t store_magic = 0x4C53544E;     // "NTSL"
constexpr uint32_t effects_magic = 0x5846544E;   // "NTFX"
constexpr uint32_t settings_magic = 0x5453544E;  // "NTST"
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

// Below the WiFi credentials sector, which is below the LED positions: the
// library, then the effects below it.
uint32_t library_offset() { return pos_config_flash_offset() - FLASH_SECTOR_SIZE - store_size; }
uint32_t effects_offset() { return library_offset() - store_size; }
uint32_t settings_offset() { return effects_offset() - FLASH_SECTOR_SIZE; }

bool clear_of_program(uint32_t offset)
{
    return XIP_BASE + offset >= reinterpret_cast<uintptr_t>(&__flash_binary_end);
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

// A region's stored form, if there's a valid one: none (never written),
// invalid (damaged), or loaded with *payload / *len set.
scene_store_load_result read_region(uint32_t offset, uint32_t magic, const uint8_t **payload, uint32_t *len)
{
    if (!clear_of_program(offset))
    {
        return scene_store_load_result::invalid;
    }
    const uint8_t *flash = reinterpret_cast<const uint8_t *>(XIP_BASE + offset);
    store_header h;
    memcpy(&h, flash, sizeof(h));
    if (h.magic != magic)
    {
        return scene_store_load_result::none;   // never written (erased flash reads 0xFF)
    }
    if (h.length > payload_max || crc32(flash + sizeof(h), h.length) != h.crc)
    {
        return scene_store_load_result::invalid;
    }
    *payload = flash + sizeof(h);
    *len = h.length;
    return scene_store_load_result::loaded;
}

// Writes the n bytes staged after the header, and checks them.
bool write_region(uint32_t offset, uint32_t magic, size_t n, const char *what)
{
    store_header h = {magic, static_cast<uint32_t>(n), crc32(staging + sizeof(store_header), n), 0};
    memcpy(staging, &h, sizeof(h));
    // Only the sectors in use: a sector erase is the slow part.
    const size_t used = sizeof(h) + n;
    const size_t bytes = (used + FLASH_SECTOR_SIZE - 1) / FLASH_SECTOR_SIZE * FLASH_SECTOR_SIZE;
    memset(staging + used, 0xFF, bytes - used);
    const uint64_t t0 = time_us_64();
    flash_write_sectors_locked(offset, staging, bytes);
    stats.last_save_us = static_cast<uint32_t>(time_us_64() - t0);

    const uint8_t *flash = reinterpret_cast<const uint8_t *>(XIP_BASE + offset);
    if (memcmp(flash, staging, used) != 0)
    {
        stats.save_failures++;
        event_logf("scene store: %s verify FAILED after writing %u bytes", what, (unsigned)n);
        return false;
    }
    stats.saves++;
    event_logf("scene store: %s saved (%u bytes, %u ms)", what, (unsigned)n, (unsigned)(stats.last_save_us / 1000));
    return true;
}

}  // namespace

scene_store_load_result scene_store_load(neotree::Library &library)
{
    // The effects first: stored scenes name them.
    const uint8_t *payload = nullptr;
    uint32_t len = 0;
    stats.effects_load = read_region(effects_offset(), effects_magic, &payload, &len);
    if (stats.effects_load == scene_store_load_result::loaded && !library.load_effects(payload, len))
    {
        stats.effects_load = scene_store_load_result::invalid;
    }
    if (stats.effects_load == scene_store_load_result::invalid)
    {
        event_logf("scene store: stored effects are damaged - none loaded");
    }
    else if (stats.effects_load == scene_store_load_result::loaded)
    {
        stats.effects_bytes = len;
        event_logf("scene store: effects loaded (%u bytes)", (unsigned)len);
    }

    stats.load = read_region(library_offset(), store_magic, &payload, &len);
    if (stats.load == scene_store_load_result::loaded && !library.load(payload, len))
    {
        stats.load = scene_store_load_result::invalid;
    }
    if (stats.load == scene_store_load_result::invalid)
    {
        event_logf("scene store: stored library is damaged - using the built-ins");
    }
    else if (stats.load == scene_store_load_result::loaded)
    {
        stats.stored_bytes = len;
        event_logf("scene store: library loaded (%u bytes)", (unsigned)len);
    }
    return stats.load;
}

bool scene_store_save(const neotree::Library &library)
{
    const size_t n =
        clear_of_program(library_offset()) ? library.save(staging + sizeof(store_header), payload_max) : 0;
    if (n == 0)
    {
        stats.save_failures++;
        event_logf("scene store: library too big to store - not saved");
        return false;
    }
    const bool ok = write_region(library_offset(), store_magic, n, "library");
    stats.stored_bytes = ok ? n : stats.stored_bytes;
    return ok;
}

bool scene_store_save_effects(const neotree::Library &library)
{
    const size_t n =
        clear_of_program(effects_offset()) ? library.save_effects(staging + sizeof(store_header), payload_max) : 0;
    if (n == 0)
    {
        stats.save_failures++;
        event_logf("scene store: effects too big to store - not saved");
        return false;
    }
    const bool ok = write_region(effects_offset(), effects_magic, n, "effects");
    stats.effects_bytes = ok ? n : stats.effects_bytes;
    return ok;
}

scene_store_stats_t scene_store_stats()
{
    return stats;
}

size_t scene_store_load_settings(char *out, size_t cap)
{
    const uint8_t *payload = nullptr;
    uint32_t len = 0;
    if (cap == 0 || read_region(settings_offset(), settings_magic, &payload, &len) != scene_store_load_result::loaded)
    {
        return 0;
    }
    const size_t n = len < cap - 1 ? len : cap - 1;
    memcpy(out, payload, n);
    out[n] = '\0';
    return n;
}

bool scene_store_save_settings(const char *text)
{
    const size_t n = strnlen(text, scene_store_settings_max + 1);
    if (n > scene_store_settings_max || !clear_of_program(settings_offset()))
    {
        stats.save_failures++;
        return false;
    }
    memcpy(staging + sizeof(store_header), text, n);
    return write_region(settings_offset(), settings_magic, n, "settings");
}
