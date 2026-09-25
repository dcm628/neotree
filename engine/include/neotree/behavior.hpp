#pragma once
// Interaction and behavior (docs/RENDERER.md section 7): collisions between
// entities, events, data-driven rules (trigger -> conditions -> actions),
// entity templates, and emitters. Everything is per slot, so stacked modes
// stay isolated; signals are the one thing that crosses slots.
//
// Runaway protection (7.4): events raised while running a tick are handled
// on the next tick, never recursively; each slot has an entity quota; each
// tick has an action budget; rules have cooldowns and fire limits. Every
// limit hit is counted in BehaviorStats.

#include <cstdint>

#include "neotree/color.hpp"
#include "neotree/entity.hpp"
#include "neotree/pool.hpp"
#include "neotree/scene.hpp"
#include "neotree/types.hpp"

namespace neotree {

class Engine;

constexpr uint8_t any_group = 0xFE;   // rule filter: matches every group
constexpr uint8_t max_templates_per_slot = 8;
constexpr uint8_t max_emitters_per_slot = 8;
constexpr uint8_t max_rules_per_slot = 16;
constexpr uint8_t max_actions_per_rule = 4;
constexpr uint16_t max_events = 96;       // per tick
constexpr uint16_t max_contacts = 128;    // touching pairs tracked across ticks
constexpr uint16_t default_slot_quota = 200;
constexpr uint16_t actions_per_tick = 96;

// ---- collisions ----

// What happens when entities of two groups meet (per slot, per pair of groups).
enum class Response : uint8_t
{
    ignore,         // no detection at all
    overlap,        // pass through each other; overlap begin/end events
    bounce,         // elastic-ish bounce (mass, restitution); collision event
    stick,          // move on together (shared momentum); collision event
    destroy_a,      // the first group's entity is removed; collision event
    destroy_b,      // the second group's
    destroy_both,
};

// ---- events ----

enum class EventType : uint8_t
{
    collision,      // contact began (bounce / stick / destroy responses)
    overlap_begin,  // started passing through (overlap response)
    overlap_end,
    boundary,       // crossed the floor / ceiling / outer envelope (id = hit_* bits)
    expired,        // lifetime ran out
    spawned,        // created by a rule or emitter
    timer,          // a timer rule came due (only that rule sees it)
    signal,         // raised by a rule's signal action (id); seen by every slot
    input,          // from outside - the app (id, value)
    count_below,    // a group's count dropped below a rule's threshold
    count_above,    // ... rose above it
};

// Everything an action might need about what happened. Entity data is copied
// in when the event is raised: the entities may be gone by the time the
// event is handled.
struct Event
{
    EventType type = EventType::signal;
    uint8_t slot = 0;
    uint8_t id = 0;                // boundary bits / signal / input id
    uint8_t group_a = no_group;
    uint8_t group_b = no_group;
    Handle a{};
    Handle b{};
    Vec3 point{};                  // contact point, or entity a's position
    Vec3 pos_a{}, pos_b{};
    Vec3 vel_a{}, vel_b{};
    Rgb color_a{}, color_b{};
    float value = 0.0f;            // input value; collision closing speed
};

// ---- rules ----

enum class Trigger : uint8_t
{
    collision,
    overlap_begin,
    overlap_end,
    boundary,       // id = which hit_* bits count (0 = any)
    expired,
    spawned,
    timer,          // delay_s, then every period_s (0 = once)
    signal,         // id
    input,          // id
    count_below,    // group_a's count drops below threshold
    count_above,    // ... rises above it
};

enum class ActionType : uint8_t
{
    spawn,          // count x template[index] at place, with velocity and color options
    destroy,        // target
    set_color,      // target, color from color_from
    set_velocity,   // target: velocity = vel
    impulse,        // target: velocity += vel
    emitter_on,     // emitter[index] (this slot)
    emitter_off,
    emitter_rate,   // emitter[index].rate = value
    set_gravity,    // global gravity = vel (mm/s^2)
    set_wind,       // global wind = vel
    layer_color,    // layer[index] of this slot: color from color_from
    layer_opacity,  // layer[index].opacity = value
    signal,         // raises signal id = index, seen by rules in every slot
    cycle,          // counts one cycle of this slot's mode (lifecycle end conditions)
    end_mode,       // ends this slot's mode now, with outcome = index (lifecycle overrides)
};

// Which entities an action applies to.
enum class Target : uint8_t
{
    a,              // the event's first entity (the rule's group_a side)
    b,
    both,
    group,          // every entity in this slot's group (Action::group)
};

// Where spawned entities appear.
enum class Place : uint8_t
{
    event_point,    // the contact point / the event entity's position
    a,
    b,
    fixed,          // Action::pos
    anywhere,       // random, inside the tree's volume
};

enum class ColorFrom : uint8_t
{
    keep,           // spawn: the template's color; set_color: unchanged
    fixed,          // Action::color
    a,              // the event's entity a's color (at the time of the event)
    b,
    mix,            // halfway between a and b
    random_hue,     // one random bright hue per action (a burst shares it)
    random_each,    // a different random hue per entity
};

struct Action
{
    ActionType type = ActionType::spawn;
    Target target = Target::a;
    uint8_t group = 0;             // Target::group
    uint8_t index = 0;             // template / emitter / layer / signal id
    uint8_t count = 1;             // spawn
    Place place = Place::event_point;
    ColorFrom color_from = ColorFrom::keep;
    Rgb color{};
    Vec3 pos{};                    // Place::fixed
    Vec3 vel{};                    // spawn: added velocity; set_velocity / impulse / set_gravity / set_wind
    float inherit = 0.0f;          // spawn: fraction of entity a's velocity added
    float spread = 0.0f;           // spawn: plus a random velocity up to this speed, any direction (mm/s)
    float value = 0.0f;            // emitter_rate, layer_opacity
};

struct Rule
{
    bool enabled = true;
    Trigger trigger = Trigger::collision;
    // Filters: which groups the event's entities must be in. For pair
    // triggers the order doesn't matter - if (b, a) matches, the roles swap
    // so Target::a is always the group_a side.
    uint8_t group_a = any_group;
    uint8_t group_b = any_group;
    uint8_t id = 0;                // boundary bits / signal / input id
    float delay_s = 0.0f;          // timer
    float period_s = 0.0f;
    uint16_t threshold = 0;        // count triggers

    // Conditions
    float probability = 1.0f;
    float cooldown_s = 0.0f;
    uint16_t max_fires = 0;        // 0 = unlimited
    uint8_t limit_group = no_group;   // only fire while that group has fewer than limit_count entities
    uint16_t limit_count = 0;

    uint8_t action_count = 0;
    Action actions[max_actions_per_rule];

    // Runtime state (reset by add_rule)
    uint16_t fired = 0;
    float cooldown_left = 0.0f;
    float timer_left = 0.0f;
    bool above = false;            // count triggers: whether the count condition held last tick

    // Builder helper: appends an action (ignored past max_actions_per_rule).
    Rule &then(const Action &action)
    {
        if (action_count < max_actions_per_rule)
        {
            actions[action_count++] = action;
        }
        return *this;
    }
};

// ---- emitters ----

enum class Region : uint8_t
{
    point,          // min
    box,            // uniform in min..max (world mm)
    band,           // cylinder band: z in [min.z, max.z], radius in [min.x, max.x] mm, any angle
    top,            // the top of the tree, anywhere within its envelope
    volume,         // anywhere inside the tree's volume
    entity,         // rides on an entity (attach), offset by min
};

struct Emitter
{
    bool active = true;
    uint8_t template_index = 0;
    float rate = 1.0f;             // spawns per second
    Region region = Region::point;
    Vec3 min{}, max{};
    Handle attach{};               // Region::entity
    Vec3 vel{};                    // added to the template's velocity
    float spread = 0.0f;           // plus a random velocity up to this speed
    float size_jitter = 0.0f;      // size scaled by 1 +- this
    float lifetime_jitter = 0.0f;  // lifetime scaled by 1 +- this
    ColorFrom color_from = ColorFrom::keep;   // keep / fixed / random_each
    Rgb color{};

    float accumulator = 0.0f;      // runtime
};

struct BehaviorStats
{
    uint32_t events = 0;               // handled
    uint32_t events_dropped = 0;       // queue full
    uint32_t rule_fires = 0;
    uint32_t actions_dropped = 0;      // over the per-tick budget
    uint32_t spawns_over_quota = 0;    // a slot at its entity quota
    uint32_t contacts_dropped = 0;     // contact table full
    uint16_t peak_entities = 0;
};

class Behavior
{
public:
    void clear();
    // Removes a slot's rules, emitters, templates, collision responses and
    // contacts, and resets its quota. (Its entities are the engine's.)
    void clear_slot(uint8_t slot);

    // Setup, per slot. Return an index (or -1 if full).
    int add_template(uint8_t slot, const Entity &entity);
    Entity *template_at(uint8_t slot, uint8_t index);
    int add_rule(uint8_t slot, const Rule &rule);
    Rule *rule(uint8_t slot, uint8_t index);
    int add_emitter(uint8_t slot, const Emitter &emitter);
    Emitter *emitter(uint8_t slot, uint8_t index);
    // Sets how groups a and b respond to each other (both orders).
    void set_response(uint8_t slot, uint8_t group_a, uint8_t group_b, Response response);
    void clear_responses(uint8_t slot);   // every pair back to ignore
    uint16_t quota(uint8_t slot) const;
    // After rules are edited in place: which events the slot's rules handle.
    void refresh_listening(uint8_t slot);
    Response response(uint8_t slot, uint8_t group_a, uint8_t group_b) const;
    void set_quota(uint8_t slot, uint16_t max_entities_in_slot);

    // Queues an event for the next tick - if any rule could handle it (events
    // nothing listens to are skipped, so they cost nothing).
    void raise(const Event &event);
    // Whether any rule in slot handles events of this type (signals: any slot).
    bool listens(uint8_t slot, EventType type) const;

    // Entities per (slot, group) and per slot, as of the last tick.
    uint16_t count(uint8_t slot, uint8_t group) const;
    uint16_t slot_count(uint8_t slot) const;

    const BehaviorStats &stats() const { return stats_; }

    // Called by the engine each tick (in this order).
    void handle_events(Engine &engine, float dt);   // last tick's events, timers, count triggers
    void run_emitters(Engine &engine, float dt);
    void collide(Engine &engine);                   // after motion
    void recount(const EntityPool &pool);

    // Spawns into a slot, honouring its quota; raises a spawned event.
    Handle spawn(Engine &engine, uint8_t slot, const Entity &entity);

private:
    struct Contact
    {
        Handle a, b;
        uint8_t slot = 0;
        bool seen = false;
    };

    struct SlotBehavior
    {
        Entity templates[max_templates_per_slot];
        uint8_t template_count = 0;
        Rule rules[max_rules_per_slot];
        uint8_t rule_count = 0;
        Emitter emitters[max_emitters_per_slot];
        uint8_t emitter_count = 0;
        Response responses[max_groups][max_groups] = {};
        uint16_t quota = default_slot_quota;
        uint16_t counts[max_groups] = {};
        uint16_t total = 0;
        uint16_t listening = 0;   // EventType bits some rule here handles
    };

    void fire(Engine &engine, uint8_t slot, Rule &rule, const Event *event, bool swapped);
    void run_action(Engine &engine, uint8_t slot, const Action &action, const Event *event, bool swapped);
    bool matches(const Rule &rule, const Event &event, bool &swapped) const;
    Contact *find_contact(Handle a, Handle b);

    SlotBehavior slots_[max_slots];
    Event queues_[2][max_events];
    uint16_t queue_len_[2] = {};
    uint8_t current_ = 0;           // queues_[current_] collects this tick's events
    Contact contacts_[max_contacts];
    uint16_t contact_count_ = 0;
    uint16_t actions_left_ = actions_per_tick;
    BehaviorStats stats_{};
};

}  // namespace neotree
