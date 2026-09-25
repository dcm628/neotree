#pragma once
// Modes (docs/RENDERER.md section 8): reusable effects that fill one slot.
// Each declares its parameters (so apps can build controls from the
// description) and a setup function that builds the slot's layers,
// templates, emitters, rules and entities from the parameter values. An
// optional tick hook runs every simulation tick, and an optional on_param
// hook applies a parameter change live (otherwise the mode is set up again).

#include <cstddef>
#include <cstdint>

#include "neotree/color.hpp"
#include "neotree/types.hpp"

namespace neotree {

class Engine;

constexpr uint8_t max_params = 6;
constexpr uint8_t no_mode = 0xFF;

enum class ParamType : uint8_t
{
    number,   // min..max, step
    color,
    choice,   // one of `choices` ("a|b|c"); value = index
    toggle,   // value 0 / 1
};

struct ParamDef
{
    const char *id;
    const char *label;
    ParamType type;
    float min = 0.0f;
    float max = 1.0f;
    float step = 0.0f;
    float def = 0.0f;           // number / choice index / toggle
    Rgb def_color{};
    const char *choices = nullptr;
};

struct ParamValue
{
    float f = 0.0f;
    Rgb c{};

    bool operator==(const ParamValue &o) const { return f == o.f && c.r == o.c.r && c.g == o.c.g && c.b == o.c.b; }
};

struct ModeContext;

struct ModeDef
{
    const char *id;
    const char *name;
    const char *summary;
    const ParamDef *params;
    uint8_t param_count;
    void (*setup)(ModeContext &ctx);
    void (*tick)(ModeContext &ctx, float dt) = nullptr;
    // Applies a change to params[index] live; return false (or leave null)
    // to have the mode set up again instead.
    bool (*on_param)(ModeContext &ctx, uint8_t index) = nullptr;
    // Direct control (neotree/direct.hpp): phones may spawn and move
    // entities in this mode's slot - a ball from template 0, a brush from
    // template 1.
    bool direct = false;
    // A brush stroke from `from` to `to` (mm) with its pen down: what the
    // mode leaves behind.
    void (*stroke)(ModeContext &ctx, Vec3 from, Vec3 to, Rgb color, float radius_mm) = nullptr;
};

struct ModeContext
{
    Engine &engine;
    uint8_t slot;
    const ModeDef &def;
    const ParamValue *values;

    float num(uint8_t i) const { return values[i].f; }
    Rgb color(uint8_t i) const { return values[i].c; }
    int choice(uint8_t i) const { return static_cast<int>(values[i].f + 0.5f); }
    bool toggle(uint8_t i) const { return values[i].f >= 0.5f; }
};

// The built-in modes. Indices are stable (the protocol uses them).
uint8_t mode_count();
const ModeDef *mode_at(uint8_t index);
// Index of the mode with this id, or no_mode.
uint8_t find_mode(const char *id);
void default_params(const ModeDef &def, ParamValue out[max_params]);

// The modes and their parameters as JSON, for apps to build controls from.
// Compact, to fit the tree's 4 KB reply frame:
//   {"modes":[{"id":"snow","n":"Snow","s":"summary","p":[PARAM...]}...],
//    "presets":["Colors",...]}
// A mode's index (for SLOT_SET) is its position in "modes"; a parameter's
// (for PARAM_SET) is its position in "p". PARAM is {"id","l" (label),"t"
// (type: "n" number, "c" color, "ch" choice, "t" toggle), then "min","max",
// "st" (step), "d" (default: number, "#rrggbb", choice index or bool), and
// "ch" ("a|b|c") for choices}.
// Returns the length written (truncated JSON if cap is too small).
size_t describe_modes(char *out, size_t cap);

}  // namespace neotree
