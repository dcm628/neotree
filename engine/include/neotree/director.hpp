#pragma once
// The director runs modes in slots (docs/RENDERER.md sections 8-9): what's
// in each slot, parameter changes, lifecycles (end conditions and what
// happens next - loop, chain, revert, remove, hold), fade transitions, whole
// scenes, and shows (presets played in turn). It owns the library: presets,
// shows and the base scene everything reverts to.
//
// Transitions are fades of the slot's opacity: the old mode fades out over
// what's below it, the new one fades in. (A true crossfade needs isolated
// slot rendering - later.)

#include <cstddef>
#include <cstdint>

#include "neotree/library.hpp"
#include "neotree/modes.hpp"
#include "neotree/scene.hpp"
#include "neotree/scene_spec.hpp"

namespace neotree {

class Engine;

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
    uint32_t show_entries = 0;        // show steps started
    uint32_t show_rounds = 0;         // times a looping show went round
    uint32_t show_skips = 0;          // entries whose preset no longer exists
};

struct ShowStatus
{
    bool playing = false;
    const char *name = "";
    const char *preset = "";          // the entry playing now
    uint8_t position = 0;             // into this round's order
    uint8_t count = 0;
    float entry_age_s = 0.0f;
    float entry_len_s = 0.0f;
    uint32_t rounds = 0;
};

class Director
{
public:
    void reset();

    Library &library() { return library_; }
    const Library &library() const { return library_; }

    // The base scene: what the tree boots into and every revert returns to.
    void set_base_scene(const SceneSpec &scene) { library_.set_base(scene); }
    const SceneSpec &base_scene() const { return library_.base(); }

    // Starts a whole scene. Slots already running the same mode with the same
    // parameters are left alone (so reverting keeps, say, the Canvas as is).
    // Untimed ignores the scene's own ending (a show times its entries).
    void apply_scene(Engine &engine, const SceneSpec &scene, Transition transition = Transition::fade,
                     bool timed = true);
    void revert_scene(Engine &engine) { apply_scene(engine, library_.base()); }

    // The live scene as a scene description (what's in each slot now, with
    // live parameter changes, lifecycles and chain targets) - to save it.
    void capture_scene(SceneSpec &out, const char *name) const;

    // Plays a library show: each entry's preset in turn for its duration;
    // then round again (reshuffled if it shuffles), or back to the base
    // scene. Returns false for an unknown or empty show. Editing the scene
    // meanwhile doesn't stop it - its next entry replaces the scene.
    bool play_show(Engine &engine, uint8_t index);
    // Stops the show, leaving the scene as it is.
    void stop_show();
    ShowStatus show_status() const;

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
    // Bumps whenever what's running changes (not as time passes) - for
    // telling apps.
    uint32_t revision() const { return revision_; }

    // Current state as JSON: {"rev":N,"scene":"holiday","slots":[{"mode":
    // "snow","state":"running",...}],"base":[...],"show":{...} or null}.
    // Returns the length written.
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

    struct ShowRun
    {
        bool active = false;
        Show show{};                  // a copy: editing or deleting it doesn't disturb the run
        uint8_t order[max_show_entries] = {};
        uint8_t pos = 0;
        float age_s = 0.0f;
        float len_s = 0.0f;
        uint32_t rounds = 0;
    };
    void order_show(Engine &engine);
    bool next_show_position(Engine &engine);
    void start_show_entry(Engine &engine);
    void end_show(Engine &engine);

    SlotRuntime slots_[max_slots];
    Library library_{};
    SceneSpec current_{};
    float scene_age_s_ = 0.0f;
    ShowRun show_{};
    uint32_t revision_ = 0;
    DirectorStats stats_{};
};

}  // namespace neotree
