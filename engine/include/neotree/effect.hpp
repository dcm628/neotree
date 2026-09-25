#pragma once
// Custom effects (docs/RENDERER.md 12.1, M8): a mode as data, built in the
// app. An effect is what a mode's setup builds in its slot - layers, things
// (entity templates), sources (emitters), what happens when groups meet,
// rules, what it starts with, forces - so running one is a generic setup
// (apply_effect), and copying a built-in mode is running its setup and
// capturing what it built (capture_effect).
//
// One schema describes every editable field (effect_section): it drives the
// app's editor, the JSON the app reads, the edits it sends and the stored
// form. Field numbers within a section are append-only.

#include <cstddef>
#include <cstdint>

#include "neotree/behavior.hpp"
#include "neotree/entity.hpp"
#include "neotree/scene_spec.hpp"
#include "neotree/scene.hpp"

namespace neotree {

class Engine;

constexpr uint8_t max_effect_rules = 12;
constexpr uint8_t max_effect_meets = 16;
constexpr uint8_t max_effect_starts = 4;
constexpr uint8_t effect_groups = 8;   // groups A-H offered in the editor

struct Meet
{
    uint8_t a = 0;
    uint8_t b = 0;
    Response response = Response::bounce;
};

enum class StartPlace : uint8_t
{
    anywhere,
    top,
    bottom,
};

// What the effect starts with: so many of a thing.
struct Start
{
    uint8_t thing = 0;
    uint8_t count = 1;
    StartPlace place = StartPlace::anywhere;
    float speed = 0.0f;          // plus a random velocity up to this, any direction (mm/s)
    bool random_colors = false;
};

struct Effect
{
    char name[name_size] = "";
    uint8_t layer_count = 0;
    Layer layers[max_layers_per_slot];
    uint8_t thing_count = 0;
    Entity things[max_templates_per_slot];
    uint8_t source_count = 0;
    Emitter sources[max_emitters_per_slot];
    uint8_t meet_count = 0;
    Meet meets[max_effect_meets];
    uint8_t rule_count = 0;
    Rule rules[max_effect_rules];   // with their actions
    uint8_t start_count = 0;
    Start starts[max_effect_starts];
    uint16_t quota = default_slot_quota;
    bool sets_forces = false;
    Forces forces{};
};

// ---- schema ----

enum class EffectSection : uint8_t
{
    settings,   // one item: quota, forces
    layers,
    things,
    sources,
    meets,
    rules,
    actions,    // a rule's actions: item index = rule * max_actions_per_rule + action
    starts,
    count
};

enum class FieldType : uint8_t
{
    number,
    color,
    choice,   // value = index into choices ("a|b|c")
    toggle,   // value 0 / 1
};

struct EffectField
{
    const char *id;
    const char *label;
    FieldType type;
    float min = 0.0f;
    float max = 1.0f;
    float step = 0.0f;
    const char *choices = nullptr;
    // Shown only when field `shown_by` (a choice or toggle in the same item)
    // has a value whose bit is set in `shown_when`; -1 = always shown.
    int8_t shown_by = -1;
    uint16_t shown_when = 0;
};

struct EffectSectionInfo
{
    const char *id;
    const char *label;     // e.g. "Things"
    const char *item;      // e.g. "Thing"
    const EffectField *fields;
    uint8_t field_count;
    uint8_t max_items;
};

const EffectSectionInfo &effect_section(EffectSection section);

// A field's value: numbers, choices and toggles in f, colors in c.
struct FieldValue
{
    float f = 0.0f;
    Rgb c{};
};

uint8_t effect_item_count(const Effect &effect, EffectSection section);   // actions: over all rules
bool effect_item_exists(const Effect &effect, EffectSection section, uint8_t index);
bool effect_get(const Effect &effect, EffectSection section, uint8_t index, uint8_t field, FieldValue &out);
bool effect_set(Effect &effect, EffectSection section, uint8_t index, uint8_t field, const FieldValue &value);
// Whether the editor shows the field (by its shown_by / shown_when, and
// that field's own).
bool effect_field_shown(const Effect &effect, EffectSection section, uint8_t index, uint8_t field);

// Structure: adds a default item (actions: to rule `index`), removes or
// duplicates item `index`. Returns false if full or out of range.
bool effect_add(Effect &effect, EffectSection section, uint8_t index);
bool effect_remove(Effect &effect, EffectSection section, uint8_t index);
bool effect_duplicate(Effect &effect, EffectSection section, uint8_t index);

// A fresh effect: a dark backdrop, a things layer, one thing and a source of
// it - something on the tree to start editing.
void effect_starter(Effect &effect);

// ---- running ----

// Builds the effect in the slot (the setup of an effect's mode).
void apply_effect(Engine &engine, uint8_t slot, const Effect &effect);
// The slot's content as an effect (what a mode's setup built there): layers
// (not pixel ones), templates, emitters, meets, rules, quota, the forces, and
// what's alive in it now as what it starts with.
void capture_effect(Engine &engine, uint8_t slot, Effect &out);
// Pushes one changed field into the slot running the effect, live: layers,
// templates and everything made from them, emitters, meets, rules, forces.
// (Starts only take effect when the effect restarts.)
void effect_apply_field(Engine &engine, uint8_t slot, const Effect &effect, EffectSection section, uint8_t index,
                        uint8_t field);

// ---- for apps and storage ----

// {"s":section,"i":[[values in schema order]...]} (actions: [rule, action,
// values...]); colors as "#rrggbb". Returns the length written.
size_t effect_section_json(const Effect &effect, EffectSection section, char *out, size_t cap);
// {"s":section,"id":..,"l":..,"item":..,"max":..,"f":[{"id","l","t","min",
// "max","st","ch","by","when"}...]} - fields at their defaults left out
// (min 0, max 1, st 0, by -1).
size_t effect_schema_json(EffectSection section, char *out, size_t cap);

// The stored form: the name, then per item only the fields that differ from
// a new item's, each with its field number and a type tag - so fields added
// later default, and unknown ones are skipped. Returns the length (0: too
// big for cap) / false on malformed data.
size_t effect_save(const Effect &effect, uint8_t *out, size_t cap);
bool effect_load(Effect &effect, const uint8_t *data, size_t len);

// One effect-sized scratch (an Effect is over 6 KB - too big for a stack),
// for decoding saved effects. Not reentrant: fill it, use it, done.
Effect &effect_scratch();

}  // namespace neotree
