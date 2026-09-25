#pragma once
// Scene descriptions (docs/RENDERER.md 9.3.1): what goes in each slot - a
// mode, its parameters, opacity and lifecycle - and whole scenes. Presets,
// shows, the base scene and the tree's stored library all hold these.

#include <cstdint>

#include "neotree/modes.hpp"

namespace neotree {

constexpr uint8_t max_specs = 8;        // per scene: 0-3 start in slots 0-3, 4-7 are chain targets
constexpr uint8_t no_spec = 0xFF;

enum class EndPolicy : uint8_t
{
    loop,     // start the same mode again (repeats times, 0 = forever; then chain if next is set, else revert)
    chain,    // start spec `next`
    revert,   // back to the base scene's mode for this slot
    remove,   // empty the slot
    hold,     // stay as it is; no further ending
};

enum class Transition : uint8_t
{
    cut,
    fade,     // out over transition_s / 2, in over transition_s / 2
};

struct Lifecycle
{
    // End conditions (any; none set = runs until changed).
    float duration_s = 0.0f;
    uint16_t cycles = 0;              // counted by the mode (ActionType::cycle)
    // On ending, emitters stop and the slot waits (up to drain_timeout_s) for
    // its entities with lifetimes (sparks, flakes) to finish before moving on.
    bool drain = true;
    float drain_timeout_s = 6.0f;

    EndPolicy policy = EndPolicy::hold;
    uint16_t repeats = 0;             // loop
    uint8_t next = no_spec;           // chain: a spec in the scene

    // An ending with this named outcome (ActionType::end_mode) goes here instead.
    uint8_t outcome = 0;              // 0 = no override
    EndPolicy outcome_policy = EndPolicy::hold;
    uint8_t outcome_next = no_spec;

    Transition transition = Transition::fade;
    float transition_s = 1.5f;
};

struct SlotSpec
{
    uint8_t mode = no_mode;           // index into the built-in modes
    ParamValue params[max_params];
    float opacity = 1.0f;
    Lifecycle life{};

    // A spec for mode id with its default parameters.
    static SlotSpec of(const char *mode_id);
    // Sets parameter `id` (by name). No-op for unknown names.
    SlotSpec &set(const char *param_id, float value);
    SlotSpec &set(const char *param_id, Rgb color);
    bool same_mode_and_params(const SlotSpec &o) const;
};

// One serializable description of a scene (docs/RENDERER.md 9.3.1).
struct SceneSpec
{
    char name[20] = "";
    SlotSpec specs[max_specs];        // 0-3: what each slot starts with; 4-7: chain targets
    // Scene-level ending: after duration_s, loop (start over), revert (to
    // the base scene) or hold.
    float duration_s = 0.0f;
    EndPolicy policy = EndPolicy::hold;
};

}  // namespace neotree
