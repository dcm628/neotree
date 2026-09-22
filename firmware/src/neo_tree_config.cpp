#include "neo_tree_config.hpp"
#include <stdio.h>
#include <cstring>
#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"  // XIP_BASE
#include "hardware/sync.h"
#include "pico/multicore.h"

// Reserve the last sector of flash for config storage, at a fixed offset
// entirely independent of wherever the linker happens to place program
// code/data. This is the standard pico-sdk pattern (matches the SDK's own
// flash_program.c example) and replaces the previous approach of a custom
// linker section, which placed the struct mid-image, only 1024-byte
// aligned (flash_range_erase() requires 4096-byte sector alignment), and
// directly adjacent to .data's flash-resident initial values - a
// full-sector erase from that misaligned offset was silently corrupting
// them on every write. This binary is ~180KB; the reserved sector sits at
// the very end of the 2MB chip, nowhere near it.
static_assert(sizeof(neo_tree_pos_config_data) <= FLASH_SECTOR_SIZE,
              "neo_tree_pos_config_data must fit in a single flash sector - "
              "the write path below only erases/programs one sector");
#define CONFIG_FLASH_TARGET_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)

// RAM-resident working copy - the single source of truth everything reads.
// Compile-time default-initialized so there's always a sane value even
// before load_pos_config_from_flash() runs.
neo_tree_pos_config_data posConfigData = get_default_tree_pos_config_data();

static const neo_tree_pos_config_data* flash_config_ptr()
{
    return reinterpret_cast<const neo_tree_pos_config_data*>(
        XIP_BASE + CONFIG_FLASH_TARGET_OFFSET);
}

void load_pos_config_from_flash()
{
    const neo_tree_pos_config_data* flash_data = flash_config_ptr();
    // A never-written (or wrong-version) sector reads back as 0xFF bytes,
    // which won't match our header fields - fall back to defaults rather
    // than trusting garbage/erased flash.
    if (flash_data->config_header.type_id == config_type::string_position &&
        flash_data->config_header.size_bytes == sizeof(neo_tree_pos_config_data) &&
        flash_data->max_pos_index == max_led_config_size)
    {
        posConfigData = *flash_data;
        printf("loaded LED position config from flash\n");
    }
    else
    {
        posConfigData = get_default_tree_pos_config_data();
        printf("no valid LED position config in flash - using defaults\n");
    }
}

string_led_config lookup_pos_config(uint16_t string_position_in)
{
    bool match_found = false;
    string_led_config return_config;
    uint32_t interrupts = save_and_disable_interrupts();
    for (auto & element : posConfigData.tree_config_array) {
        if (element.string_position == string_position_in)
        {
            match_found = true;
            return_config = element;
        }
    }
    if (match_found == false)
    {
    return_config.string_position = 0;
    return_config.coordinates.omega = 0;
    return_config.coordinates.radius = 0;
    return_config.coordinates.z = 0;
    }
    restore_interrupts(interrupts);
    return return_config;
}

void print_pos_config(uint16_t string_position_in)
{
    string_led_config config = lookup_pos_config(string_position_in);
    // Fixed, parseable format so this can be read either by eye over a
    // serial monitor or by a script watching for it after sending a write.
    printf("POS_CONFIG position: %u omega: %u radius: %u z: %d\n",
           config.string_position, config.coordinates.omega,
           config.coordinates.radius, config.coordinates.z);
}

bool verify_pos_config(string_led_config set_config, string_led_config read_config)
{
    // Was comparing every set_config field against itself (always false,
    // meaning "always matches") - this function always returned true
    // regardless of what was actually written. Compare against what was
    // actually read back instead.
    bool matches = (set_config.string_position == read_config.string_position)
        && (set_config.coordinates.omega == read_config.coordinates.omega)
        && (set_config.coordinates.radius == read_config.coordinates.radius)
        && (set_config.coordinates.z == read_config.coordinates.z);
    return matches;
}

bool write_flash_pos_config(string_led_config set_config)
{
    printf("write_flash_pos_config: position %u\n", set_config.string_position);
    if (set_config.string_position >= max_led_config_size)
    {
        printf("write_flash_pos_config: position %u out of bounds (max %u)\n",
               set_config.string_position, (unsigned)max_led_config_size);
        return false;
    }

    // Update the RAM working copy first. This is what lookup_pos_config()
    // and everything else actually reads, so the new value takes effect
    // immediately even if something below goes wrong - only a future
    // reboot before a successful flash write would lose it.
    posConfigData.tree_config_array[set_config.string_position] = set_config;

    // Stage the full sector to write. This buffer is ~4KB - it MUST be
    // static (BSS), never a stack local: each core's entire stack is only
    // 4096 bytes total (see memmap_custom.ld SCRATCH_X/SCRATCH_Y), so a
    // 4KB local here would consume essentially the whole stack with zero
    // margin for anything else, guaranteeing a stack overflow. (The
    // previous version of this function did exactly that.)
    static uint8_t staging[FLASH_SECTOR_SIZE];
    memset(staging, 0xFF, sizeof(staging));
    memcpy(staging, &posConfigData, sizeof(posConfigData));

    // Pause core1 for the duration of the erase/program. Both cores
    // execute from flash (XIP) continuously, and core1 in particular is
    // always spinning in its serial-read loop with no idle time.
    // hardware/flash.h documents that flash_range_erase/program are
    // *unsafe* if the other core can fetch from flash concurrently, and
    // recommends exactly this: the multicore_lockout functions.
    // save_and_disable_interrupts() alone (the previous approach) only
    // protects the core that calls it - core1 was never paused, so it
    // could fault mid-instruction-fetch the moment XIP was suspended.
    multicore_lockout_start_blocking();
    uint32_t interrupts = save_and_disable_interrupts();

    flash_range_erase(CONFIG_FLASH_TARGET_OFFSET, FLASH_SECTOR_SIZE);
    // Program the FULL sector, not just one page. The struct is 4024
    // bytes; the previous version erased a full 4096-byte sector but only
    // programmed back 256 bytes (one page), leaving ~3770 bytes - most of
    // the LED position array - as erased/blank flash after every write.
    flash_range_program(CONFIG_FLASH_TARGET_OFFSET, staging, FLASH_SECTOR_SIZE);

    restore_interrupts(interrupts);
    multicore_lockout_end_blocking();

    // Verify against the actual flash content (not the RAM copy we set
    // ourselves above, which would trivially "match" regardless of
    // whether the flash write succeeded).
    bool write_success = verify_pos_config(
        set_config, flash_config_ptr()->tree_config_array[set_config.string_position]);
    printf("write_flash_pos_config: verify %s\n", write_success ? "OK" : "FAILED");
    return write_success;
}
