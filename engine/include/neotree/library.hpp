#pragma once
// The library (docs/RENDERER.md 9.2-9.3.1): presets (named scenes), shows
// (presets played in turn), the base scene and the startup show.
//
// Built-in presets and shows come with the firmware. What people make - user
// presets and shows, a custom base scene, the startup show - is what the tree
// stores (save / load: a compact, versioned binary form). Stored scenes name
// modes and parameters by id, so they survive firmware updates: an unknown
// mode leaves its slot empty, an unknown parameter is ignored, a missing one
// takes its default.

#include <cstddef>
#include <cstdint>

#include "neotree/scene_spec.hpp"

namespace neotree {

struct Effect;

constexpr uint8_t max_user_presets = 8;
constexpr uint8_t max_user_shows = 4;
constexpr uint8_t max_show_entries = 16;
constexpr uint8_t no_index = 0xFF;
// Custom effects (neotree/effect.hpp) the library holds, each in its stored
// form in at most this many bytes.
constexpr uint8_t max_effects = 6;
constexpr size_t max_effect_bytes = 2048;
// A show entry with no duration, of a preset with none, plays this long.
constexpr uint16_t default_entry_s = 300;

struct ShowEntry
{
    char preset[name_size] = "";      // by name, so deleting another preset doesn't shift it
    uint16_t duration_s = 0;          // 0 = the preset's own duration (else default_entry_s)
};

struct Show
{
    char name[name_size] = "";
    ShowEntry entries[max_show_entries];
    uint8_t count = 0;
    bool loop = true;                 // else: back to the base scene after the last entry
    bool shuffle = false;             // a new random order every round
};

// The built-in presets and shows.
uint8_t preset_count();
const SceneSpec &preset_at(uint8_t index);
uint8_t find_preset(const char *name);   // no_index if none
uint8_t builtin_show_count();
const Show &builtin_show_at(uint8_t index);

// Empty a scene / show in place. (Assigning SceneSpec{} or Show{} builds a
// temporary copy on the stack - over 1 KB for a scene.)
void clear_scene(SceneSpec &scene);
void clear_show(Show &show);

// Copies up to name_size - 1 bytes of a name, dropping control characters
// and trimming spaces. Returns false if nothing is left.
bool copy_name(char (&dst)[name_size], const char *src, size_t src_len);

class Library
{
public:
    // Built-ins only; the base scene is the first preset (Colors).
    void reset();

    // Presets: the built-ins, then the user's.
    uint8_t preset_count() const;
    const SceneSpec &preset(uint8_t index) const;
    bool preset_is_user(uint8_t index) const;
    uint8_t find_preset(const char *name) const;
    // Stores a scene as a user preset: replaces the user preset with this
    // name, else adds one. Returns its index, or no_index if the library is
    // full or the name is empty or a built-in's.
    uint8_t save_preset(const SceneSpec &scene, const char *name);
    bool delete_preset(uint8_t index);   // user presets only

    // Shows, the same way.
    uint8_t show_count() const;
    const Show &show(uint8_t index) const;
    bool show_is_user(uint8_t index) const;
    uint8_t find_show(const char *name) const;
    uint8_t save_show(const Show &show);
    bool delete_show(uint8_t index);

    // The base scene: what the tree boots into and every revert returns to.
    const SceneSpec &base() const { return base_; }
    bool base_is_custom() const { return base_custom_; }
    void set_base(const SceneSpec &scene);
    void reset_base();

    // The show played at startup (by name), no_index for none.
    uint8_t boot_show() const { return find_show(boot_show_); }
    bool set_boot_show(uint8_t index);

    // Custom effects, at fixed positions 0..max_effects-1 (an effect's mode
    // index follows its position, so it stays put while others come and go).
    // Kept in their stored form; effect() decodes one. reset() leaves them:
    // they're stored apart from the rest (save_effects / load_effects).
    bool effect_used(uint8_t k) const { return k < max_effects && effects_[k].len > 0; }
    const char *effect_name(uint8_t k) const { return k < max_effects ? effects_[k].name : ""; }
    uint8_t find_effect(const char *name) const;   // no_index if none
    bool effect(uint8_t k, Effect &out) const;
    // Stores an effect under a name (which it takes): replaces the effect
    // with that name, else takes a free position. Returns the position, or
    // no_index if full, too big, or the name is empty.
    uint8_t save_effect(Effect &effect, const char *name);
    bool delete_effect(uint8_t k);
    void clear_effects();
    uint32_t effects_revision() const { return effects_revision_; }
    // Changes when anything but the effects does (every effects change bumps
    // both counters) - for storing the rest apart from them.
    uint32_t scenes_revision() const { return revision_ - effects_revision_; }
    size_t save_effects(uint8_t *out, size_t cap) const;
    // On malformed data, no effects and false.
    bool load_effects(const uint8_t *data, size_t len);

    // Bumps on every change (effects too) - for knowing when to store and to
    // tell apps.
    uint32_t revision() const { return revision_; }

    // The user's part of the library in its stored form. Returns the length,
    // or 0 if it doesn't fit in cap.
    size_t save(uint8_t *out, size_t cap) const;
    // Replaces the user's part from its stored form. On malformed data the
    // library is left reset to the built-ins and it returns false.
    bool load(const uint8_t *data, size_t len);

    // For apps: {"rev":N,"presets":[{"n":"Colors"},{"n":"Mine","u":1}...],
    // "shows":[{"n":..,"u":1,"loop":1,"shuffle":0,"e":[[preset index or -1,
    // seconds]...]}...],"boot":"show name","base":{"custom":0,"slots":[mode
    // ids]},"fx":[{"n":name,"k":position,"i":mode index}...]}. Returns the
    // length written.
    size_t describe(char *out, size_t cap) const;

private:
    SceneSpec user_presets_[max_user_presets];
    uint8_t user_preset_count_ = 0;
    Show user_shows_[max_user_shows];
    uint8_t user_show_count_ = 0;
    SceneSpec base_{};
    bool base_custom_ = false;
    char boot_show_[name_size] = "";
    uint32_t revision_ = 0;

    struct StoredEffect
    {
        char name[name_size] = "";
        uint16_t len = 0;   // 0: free
        uint8_t data[max_effect_bytes];
    };
    StoredEffect effects_[max_effects];
    uint32_t effects_revision_ = 0;
};

}  // namespace neotree
