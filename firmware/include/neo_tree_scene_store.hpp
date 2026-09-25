#ifndef _NEO_TREE_SCENE_STORE_HPP
#define _NEO_TREE_SCENE_STORE_HPP

// Keeps the engine's library - user presets and shows, a custom base scene,
// the startup show - in flash, so they survive power cuts and firmware
// updates (docs/RENDERER.md 9.3.1).
//
// Flash layout, from the top: LED positions (neo_tree_config), WiFi
// credentials (one sector, neo_tree_wifi), then this store's 4 sectors:
// [magic][length][crc32][reserved] + the library's stored form.

#include <cstddef>
#include <cstdint>

namespace neotree {
class Library;
}

enum class scene_store_load_result : uint8_t
{
    none,      // nothing stored yet (or erased): built-ins only
    loaded,
    invalid,   // damaged or unreadable: built-ins only
};

struct scene_store_stats_t
{
    scene_store_load_result load;
    uint32_t stored_bytes;    // the library's stored form, last load or save
    uint32_t saves;
    uint32_t save_failures;   // too big for the store, or verify failed
    uint32_t last_save_us;    // how long the last write held both cores
};

// At boot, before the scene starts. Leaves the library at its built-ins
// unless a valid one is stored.
scene_store_load_result scene_store_load(neotree::Library &library);

// Writes the library to flash. core0 only: it pauses core1 (and so WiFi)
// while the flash is erased and written - tens of milliseconds a sector.
bool scene_store_save(const neotree::Library &library);

scene_store_stats_t scene_store_stats();

#endif
