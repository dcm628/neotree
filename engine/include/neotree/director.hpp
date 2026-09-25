#pragma once
// The director runs modes in slots (docs/RENDERER.md sections 8-9): what's
// in each slot, parameter changes, lifecycles (end conditions and what
// happens next - loop, chain, revert, remove, hold), fade transitions, and
// whole scenes, including the base scene everything reverts to.
//
// Transitions are fades of the slot's opacity: the old mode fades out over
// what's below it, the new one fades in. (A true crossfade needs isolated
// slot rendering - later.)

#include <cstddef>
#include <cstdint>

#include "neotree/modes.hpp"
#include "neotree/scene.hpp"

namespace neotree {

class Engine;

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

enum class SlotState : uint8_t
{
    empty,
    entering,   // fading in
    running,
    ending,     // emitters off, draining
    leaving,    // fading out before the next thing starts
};

enum class EndReason : uint8_t
{
    duration,
    cycles,
    outcome,    // a rule's end_mode action
    request,    // the app / a command
    count
};

struct DirectorStats
{
    uint32_t starts[32] = {};         // per mode index
    uint32_t ends[static_cast<int>(EndReason::count)] = {};
    uint32_t drain_timeouts = 0;
    uint32_t scene_loops = 0;
};

class Director
{
public:
    void reset();

    // The base scene: what the tree boots into and every revert returns to.
    void set_base_scene(const SceneSpec &scene) { base_ = scene; }
    const SceneSpec &base_scene() const { return base_; }

    // Starts a whole scene. Slots already running the same mode with the same
    // parameters are left alone (so reverting keeps, say, the Canvas as is).
    void apply_scene(Engine &engine, const SceneSpec &scene, Transition transition = Transition::fade);
    void revert_scene(Engine &engine) { apply_scene(engine, base_); }

    // Puts a mode in a slot (fading out whatever is there first). An empty
    // spec (mode no_mode) removes.
    void set_slot(Engine &engine, uint8_t slot, const SlotSpec &spec, Transition transition = Transition::fade);
    // Changes a running mode's parameter (live if the mode supports it,
    // otherwise by setting it up again).
    bool set_param(Engine &engine, uint8_t slot, uint8_t index, const ParamValue &value);
    // Sets what a running slot does when it ends, with an explicit chain
    // target (the protocol's way; scenes use spec indices instead).
    void set_lifecycle(uint8_t slot, const Lifecycle &life, const SlotSpec *chain_to);
    // Ends a slot now (its policy applies; outcome selects an override).
    void end_slot(Engine &engine, uint8_t slot, uint8_t outcome, EndReason reason);
    // Starts the slot's current mode over / sends it back to the base scene's.
    void restart_slot(Engine &engine, uint8_t slot);
    void revert_slot(Engine &engine, uint8_t slot);
    // Called by the mode's rules (ActionType::cycle).
    void count_cycle(uint8_t slot);

    // Called by the engine first thing each tick.
    void tick(Engine &engine, float dt);

    struct SlotInfo
    {
        SlotState state = SlotState::empty;
        SlotSpec spec{};              // what's running (with live parameter changes)
        float age_s = 0.0f;
        uint16_t cycles_done = 0;
        uint16_t loops_done = 0;
    };
    const SlotInfo &slot(uint8_t s) const { return slots_[s].info; }
    const DirectorStats &stats() const { return stats_; }

    // Current state as JSON: {"slots":[{"mode":"snow","state":"running",...}],
    // "scene":"holiday","base":[...]}. Returns the length written.
    size_t describe_state(char *out, size_t cap) const;

private:
    struct SlotRuntime
    {
        SlotInfo info;
        float phase_t = 0.0f;         // time into the current fade / drain
        float phase_len = 0.0f;
        bool pending = false;         // start pending_spec when the fade-out ends
        SlotSpec pending_spec{};
        Transition pending_transition = Transition::fade;
        bool has_chain = false;       // set_lifecycle's explicit chain target
        SlotSpec chain_spec{};
        EndPolicy resolved = EndPolicy::hold;
        uint8_t resolved_next = no_spec;
        bool held = false;
    };

    void start(Engine &engine, uint8_t slot, const SlotSpec &spec, Transition transition);
    void setup_now(Engine &engine, uint8_t slot, const SlotSpec &spec, Transition transition);
    void begin_ending(Engine &engine, uint8_t slot, EndPolicy policy, uint8_t next);
    void finish_ending(Engine &engine, uint8_t slot);
    void clear_slot_content(Engine &engine, uint8_t slot);
    void set_opacity(Engine &engine, uint8_t slot, float k);

    SlotRuntime slots_[max_slots];
    SceneSpec base_{};
    SceneSpec current_{};
    float scene_age_s_ = 0.0f;
    DirectorStats stats_{};
};

// ---- presets: named scenes ----

uint8_t preset_count();
const SceneSpec &preset_at(uint8_t index);
// Index of the preset with this name, or 0xFF.
uint8_t find_preset(const char *name);

}  // namespace neotree
