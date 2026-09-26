#include "neotree/director.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "neotree/engine.hpp"
#include "json.hpp"

namespace neotree {

namespace {

using detail::Json;

const char *state_name(SlotState s)
{
    switch (s)
    {
    case SlotState::empty: return "empty";
    case SlotState::entering: return "entering";
    case SlotState::running: return "running";
    case SlotState::ending: return "ending";
    case SlotState::leaving: return "leaving";
    }
    return "?";
}

const char *policy_name(EndPolicy p)
{
    switch (p)
    {
    case EndPolicy::loop: return "loop";
    case EndPolicy::chain: return "chain";
    case EndPolicy::revert: return "revert";
    case EndPolicy::remove: return "remove";
    case EndPolicy::hold: return "hold";
    }
    return "?";
}

// Entities in the slot that will finish by themselves (they have a
// lifetime). Draining waits only for these - a sweep's band or a bouncing
// ball would never go.
uint16_t finishing_entities(const Engine &engine, uint8_t slot)
{
    const EntityPool &pool = engine.entities();
    uint16_t n = 0;
    for (uint16_t k = 0; k < pool.capacity; k++)
    {
        if (pool.alive(k) && pool.item(k).slot == slot && pool.item(k).lifetime_s > 0.0f)
        {
            n++;
        }
    }
    return n;
}

const char *mode_id(uint8_t mode)
{
    const ModeDef *def = mode_at(mode);
    return def != nullptr ? def->id : "";
}

int param_index(const ModeDef *def, const char *id)
{
    for (uint8_t i = 0; def != nullptr && i < def->param_count; i++)
    {
        if (std::strcmp(def->params[i].id, id) == 0)
        {
            return i;
        }
    }
    return -1;
}

}  // namespace

// ---- SlotSpec ----

SlotSpec SlotSpec::of(const char *mode_id_str)
{
    SlotSpec s;
    s.mode = find_mode(mode_id_str);
    if (const ModeDef *def = mode_at(s.mode))
    {
        default_params(*def, s.params);
    }
    return s;
}

SlotSpec &SlotSpec::set(const char *param_id, float value)
{
    int i = param_index(mode_at(mode), param_id);
    if (i >= 0)
    {
        params[i].f = value;
    }
    return *this;
}

SlotSpec &SlotSpec::set(const char *param_id, Rgb color)
{
    int i = param_index(mode_at(mode), param_id);
    if (i >= 0)
    {
        params[i].c = color;
    }
    return *this;
}

bool SlotSpec::same_mode_and_params(const SlotSpec &o) const
{
    if (mode != o.mode || opacity != o.opacity)
    {
        return false;
    }
    const ModeDef *def = mode_at(mode);
    for (uint8_t i = 0; def != nullptr && i < def->param_count; i++)
    {
        if (!(params[i] == o.params[i]))
        {
            return false;
        }
    }
    return true;
}

// ---- Director ----

void Director::reset()
{
    for (SlotRuntime &rt : slots_)
    {
        rt = SlotRuntime{};
    }
    library_.reset();
    current_ = SceneSpec{};
    scene_age_s_ = 0.0f;
    show_.active = false;
    revision_++;
    stats_ = {};
    effect_starter(draft_);
    draft_revision_++;
    bind_effects(&draft_, &library_);
    lights_on_ = true;
    timer_level_ = -2;
    schedule_started_ = false;
    event_running_ = -1;
}

void Director::set_opacity(Engine &engine, uint8_t slot, float k)
{
    if (Slot *s = engine.scene().slot(slot))
    {
        s->opacity = std::clamp(k, 0.0f, 1.0f) * slots_[slot].info.spec.opacity;
    }
}

void Director::clear_slot_content(Engine &engine, uint8_t slot)
{
    // A Canvas leaving keeps its pixels for when a Canvas comes back.
    const SlotRuntime &rt = slots_[slot];
    if (rt.info.state != SlotState::empty && rt.info.spec.mode == find_mode("canvas"))
    {
        auto bg = engine.scene().pixels(slot, 0);
        auto fg = engine.scene().pixels(slot, 1);
        Engine::CanvasMemory &memory = engine.canvas_memory();
        for (size_t i = 0; i < bg.size() && i < fg.size(); i++)
        {
            memory.background[i] = bg[i];
            memory.paint[i] = fg[i];
        }
        memory.saved = !bg.empty() && !fg.empty();
    }
    engine.scene().clear_slot(slot);
    engine.destroy_entities_in_slot(slot);
    engine.behavior().clear_slot(slot);
}

void Director::setup_now(Engine &engine, uint8_t slot, const SlotSpec &spec, Transition transition)
{
    SlotRuntime &rt = slots_[slot];
    clear_slot_content(engine, slot);
    rt.info.spec = spec;
    rt.info.age_s = 0.0f;
    rt.info.cycles_done = 0;
    rt.held = false;
    rt.pending = false;
    revision_++;
    const ModeDef *def = mode_at(spec.mode);
    if (def == nullptr)
    {
        rt.info.state = SlotState::empty;
        return;
    }
    ModeContext ctx{engine, slot, *def, rt.info.spec.params};
    def->setup(ctx);
    if (spec.mode < 32)
    {
        stats_.starts[spec.mode]++;
    }
    engine.note("slot %u: %s started", (unsigned)slot, def->id);
    if (transition == Transition::fade && spec.life.transition_s > 0.0f)
    {
        rt.info.state = SlotState::entering;
        rt.phase_t = 0.0f;
        rt.phase_len = spec.life.transition_s * 0.5f;
        set_opacity(engine, slot, 0.0f);
    }
    else
    {
        rt.info.state = SlotState::running;
        set_opacity(engine, slot, 1.0f);
    }
}

void Director::start(Engine &engine, uint8_t slot, const SlotSpec &spec, Transition transition)
{
    SlotRuntime &rt = slots_[slot];
    if (rt.info.state == SlotState::empty || transition == Transition::cut || spec.life.transition_s <= 0.0f)
    {
        setup_now(engine, slot, spec, transition);
        return;
    }
    // Fade the current mode out first; the new one starts when it's gone.
    revision_++;
    rt.pending = true;
    rt.pending_spec = spec;
    rt.pending_transition = transition;
    rt.phase_len = spec.life.transition_s * 0.5f;
    if (rt.info.state != SlotState::leaving)
    {
        rt.phase_t = 0.0f;
        rt.info.state = SlotState::leaving;
    }
}

void Director::apply_scene(Engine &engine, const SceneSpec &scene, Transition transition, bool timed)
{
    current_ = scene;   // (scene may be current_ itself: a scene looping)
    if (!timed)
    {
        current_.duration_s = 0.0f;
    }
    scene_age_s_ = 0.0f;
    revision_++;
    for (uint8_t s = 0; s < max_slots; s++)
    {
        SlotRuntime &rt = slots_[s];
        const SlotSpec &spec = scene.specs[s];
        bool live = rt.info.state == SlotState::running || rt.info.state == SlotState::entering;
        if (live && rt.info.spec.same_mode_and_params(spec))
        {
            rt.info.spec.life = spec.life;   // keep it running; adopt the scene's lifecycle
            rt.has_chain = false;
            rt.held = false;
            continue;
        }
        if (spec.mode == no_mode && rt.info.state == SlotState::empty)
        {
            continue;
        }
        rt.has_chain = false;
        rt.info.loops_done = 0;
        start(engine, s, spec, transition);
    }
    engine.note("scene: %s", scene.name);
}

void Director::set_slot(Engine &engine, uint8_t slot, const SlotSpec &spec, Transition transition)
{
    if (slot >= max_slots)
    {
        return;
    }
    slots_[slot].has_chain = false;
    slots_[slot].info.loops_done = 0;
    start(engine, slot, spec, transition);
}

bool Director::set_param(Engine &engine, uint8_t slot, uint8_t index, const ParamValue &value)
{
    if (slot >= max_slots)
    {
        return false;
    }
    SlotRuntime &rt = slots_[slot];
    const ModeDef *def = mode_at(rt.info.spec.mode);
    if (def == nullptr || index >= def->param_count || rt.info.state == SlotState::empty)
    {
        return false;
    }
    rt.info.spec.params[index] = value;
    revision_++;
    ModeContext ctx{engine, slot, *def, rt.info.spec.params};
    if (def->on_param != nullptr && def->on_param(ctx, index))
    {
        return true;
    }
    set_up_again(engine, slot);   // with the new value
    return true;
}

void Director::set_up_again(Engine &engine, uint8_t slot)
{
    SlotRuntime &rt = slots_[slot];
    const float age_s = rt.info.age_s;
    const uint16_t cycles_done = rt.info.cycles_done;
    const uint16_t loops_done = rt.info.loops_done;
    const SlotState state = rt.info.state;
    const float opacity_k = engine.scene().slot(slot)->opacity;
    setup_now(engine, slot, rt.info.spec, Transition::cut);
    rt.info.age_s = age_s;
    rt.info.cycles_done = cycles_done;
    rt.info.loops_done = loops_done;
    rt.info.state = state;
    engine.scene().slot(slot)->opacity = opacity_k;
}

// ---- the schedule ----

namespace {

int64_t floor_div64(int64_t a, int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }

// Events fire as their moment passes; a clock that jumps further than this
// (set for the first time, corrected by a lot) skips what it jumped over.
constexpr int64_t event_grace_ms = 5000;

}  // namespace

void Director::apply_lights(Engine &engine, bool on)
{
    engine.master().output_enabled = on;
    if (lights_on_ != on)
    {
        lights_on_ = on;
        revision_++;
    }
}

void Director::set_lights(Engine &engine, bool on)
{
    apply_lights(engine, on);
    engine.note("lights %s", on ? "on" : "off");
}

void Director::run_schedule(Engine &engine, int64_t unix_ms, const TimeZone &zone)
{
    const int64_t unix_s = floor_div64(unix_ms, 1000);
    const int32_t offset_s = in_dst(unix_s, zone) ? zone.dst_offset_s : zone.std_offset_s;
    const int64_t local_ms = unix_ms + static_cast<int64_t>(offset_s) * 1000;
    const Schedule &s = library_.schedule();

    // The on/off timer: acts when what it says changes (or the schedule
    // does, or on the first call); by hand in between wins until then.
    const int level = timer_level(s, floor_div64(local_ms, 1000));
    const bool edited = library_.schedule_revision() != schedule_seen_;
    if (level != timer_level_ || edited || !schedule_started_)
    {
        const bool changed = level != timer_level_;
        timer_level_ = level;
        schedule_seen_ = library_.schedule_revision();
        if (changed)
        {
            revision_++;
        }
        if (level >= 0 && event_running_ < 0 && (changed || edited || !schedule_started_))
        {
            if ((level == 1) != lights_on_)
            {
                engine.note("timer: lights %s", level == 1 ? "on" : "off");
            }
            apply_lights(engine, level == 1);
        }
    }

    // Events whose moment passed since the last call.
    const int64_t step = unix_ms - prev_unix_ms_;
    if (schedule_started_ && step > 0 && step <= event_grace_ms && local_ms > prev_local_ms_)
    {
        const int64_t first_day = floor_div64(prev_local_ms_, 86400000);
        const int64_t last_day = floor_div64(local_ms, 86400000);
        for (int64_t day = first_day; day <= last_day && day <= first_day + 1; day++)
        {
            for (uint8_t i = 0; i < s.event_count; i++)
            {
                const ScheduledEvent &e = s.events[i];
                const int64_t at = day * 86400000 + static_cast<int64_t>(e.time_s) * 1000;
                if (e.enabled && event_on_day(e, day) && at > prev_local_ms_ && at <= local_ms)
                {
                    start_event(engine, i);
                }
            }
        }
    }
    schedule_started_ = true;
    prev_unix_ms_ = unix_ms;
    prev_local_ms_ = local_ms;
}

bool Director::start_event(Engine &engine, uint8_t index)
{
    const Schedule &s = library_.schedule();
    if (index >= s.event_count)
    {
        return false;
    }
    const ScheduledEvent &e = s.events[index];
    // What it plays, checked before anything changes.
    uint8_t target = no_index;
    switch (e.action)
    {
    case EventAction::preset: target = library_.find_preset(e.target); break;
    case EventAction::show: target = library_.find_show(e.target); break;
    case EventAction::mode: target = find_mode(e.target) == no_mode ? no_index : find_mode(e.target); break;
    }
    if (target == no_index)
    {
        engine.note("event %s: no %s \"%s\"", e.name, e.action == EventAction::mode ? "mode" : "scene or show",
                    e.target);
        return false;
    }
    // What to go back to (the first event's, if one replaces another).
    if (event_running_ < 0)
    {
        capture_scene(event_before_, current_.name);
        std::snprintf(event_before_show_, sizeof(event_before_show_), "%s", show_.active ? show_.show.name : "");
        event_before_lights_ = lights_on_;
    }
    switch (e.action)
    {
    case EventAction::preset:
        stop_show();
        apply_scene(engine, library_.preset(target));
        break;
    case EventAction::show:
        play_show(engine, target);
        break;
    case EventAction::mode:
    {
        stop_show();
        SlotSpec spec{};
        spec.mode = target;
        set_slot(engine, 1, spec, Transition::fade);
        break;
    }
    }
    apply_lights(engine, true);
    engine.note("event: %s", e.name);
    event_running_ = e.duration_s > 0 ? index : -1;   // 0: it stays; nothing to go back to
    event_left_s_ = static_cast<float>(e.duration_s);
    revision_++;
    return true;
}

void Director::end_event(Engine &engine)
{
    if (event_running_ < 0)
    {
        return;
    }
    event_running_ = -1;
    revision_++;
    stop_show();
    const uint8_t show = event_before_show_[0] != '\0' ? library_.find_show(event_before_show_) : no_index;
    if (show != no_index)
    {
        play_show(engine, show);
    }
    else
    {
        apply_scene(engine, event_before_);
    }
    // The lights: as the timer says now, else as they were.
    apply_lights(engine, timer_level_ >= 0 ? timer_level_ == 1 : event_before_lights_);
    engine.note("event over: back to before");
}

// ---- custom effects ----

int Director::draft_slot() const
{
    for (uint8_t s = 0; s < max_slots; s++)
    {
        const SlotRuntime &rt = slots_[s];
        if (rt.info.state != SlotState::empty && rt.info.state != SlotState::leaving && rt.info.spec.mode == draft_mode)
        {
            return s;
        }
    }
    return -1;
}

bool Director::edit_effect(Engine &engine, uint8_t slot, uint8_t mode)
{
    if (slot >= max_slots)
    {
        return false;
    }
    if (mode == no_mode)
    {
        effect_starter(draft_);
    }
    else if (is_effect_mode(mode) && mode != draft_mode)
    {
        if (!library_.effect(static_cast<uint8_t>(mode - effect_mode(0)), draft_))
        {
            return false;
        }
    }
    else if (mode != draft_mode)
    {
        // A built-in: set it up here and take what it built.
        const ModeDef *def = mode_at(mode);
        if (def == nullptr)
        {
            return false;
        }
        const SlotRuntime &rt = slots_[slot];
        ParamValue params[max_params];
        if (rt.info.state != SlotState::empty && rt.info.spec.mode == mode)
        {
            std::memcpy(params, rt.info.spec.params, sizeof(params));
        }
        else
        {
            default_params(*def, params);
        }
        clear_slot_content(engine, slot);
        ModeContext ctx{engine, slot, *def, params};
        def->setup(ctx);
        capture_effect(engine, slot, draft_);
        std::snprintf(draft_.name, sizeof(draft_.name), "%s", def->name);
    }
    draft_revision_++;
    // The draft runs in this slot only.
    for (uint8_t s = 0; s < max_slots; s++)
    {
        if (s != slot && slots_[s].info.state != SlotState::empty && slots_[s].info.spec.mode == draft_mode)
        {
            set_slot(engine, s, SlotSpec{}, Transition::cut);
        }
    }
    SlotSpec spec{};
    spec.mode = draft_mode;
    set_slot(engine, slot, spec, Transition::cut);
    return true;
}

bool Director::edit_field(Engine &engine, EffectSection section, uint8_t index, uint8_t field,
                          const FieldValue &value)
{
    if (!effect_set(draft_, section, index, field, value))
    {
        return false;
    }
    draft_revision_++;
    const int s = draft_slot();
    if (s < 0)
    {
        return true;
    }
    // What can't change in place: what it starts with, a layer turning to or
    // from things, a thing moving to another layer.
    const bool rebuild = section == EffectSection::starts || (section == EffectSection::layers && field == 0) ||
                         (section == EffectSection::things && field == 9);
    if (rebuild)
    {
        set_up_again(engine, static_cast<uint8_t>(s));
    }
    else
    {
        effect_apply_field(engine, static_cast<uint8_t>(s), draft_, section, index, field);
    }
    return true;
}

bool Director::edit_item(Engine &engine, uint8_t op, EffectSection section, uint8_t index)
{
    bool ok = false;
    switch (op)
    {
    case 0: ok = effect_add(draft_, section, index); break;
    case 1: ok = effect_remove(draft_, section, index); break;
    case 2: ok = effect_duplicate(draft_, section, index); break;
    default: break;
    }
    if (!ok)
    {
        return false;
    }
    draft_revision_++;
    const int s = draft_slot();
    if (s >= 0)
    {
        set_up_again(engine, static_cast<uint8_t>(s));
    }
    return true;
}

uint8_t Director::save_draft(const char *name)
{
    const uint8_t k = library_.save_effect(draft_, name);
    if (k != no_index)
    {
        draft_revision_++;
        revision_++;   // the running draft's name
    }
    return k;
}

void Director::set_lifecycle(uint8_t slot, const Lifecycle &life, const SlotSpec *chain_to)
{
    if (slot >= max_slots)
    {
        return;
    }
    SlotRuntime &rt = slots_[slot];
    rt.info.spec.life = life;
    revision_++;
    rt.has_chain = chain_to != nullptr;
    if (chain_to != nullptr)
    {
        rt.chain_spec = *chain_to;
    }
    rt.held = false;
}

void Director::end_slot(Engine &engine, uint8_t slot, uint8_t outcome, EndReason reason)
{
    if (slot >= max_slots)
    {
        return;
    }
    SlotRuntime &rt = slots_[slot];
    if (rt.info.state != SlotState::running && rt.info.state != SlotState::entering)
    {
        return;
    }
    stats_.ends[static_cast<int>(reason)]++;
    const Lifecycle &life = rt.info.spec.life;
    if (outcome != 0 && outcome == life.outcome)
    {
        begin_ending(engine, slot, life.outcome_policy, life.outcome_next);
    }
    else if (reason == EndReason::request && life.policy == EndPolicy::hold)
    {
        begin_ending(engine, slot, EndPolicy::revert, no_spec);   // asked to end: don't just hold
    }
    else
    {
        begin_ending(engine, slot, life.policy, life.next);
    }
}

void Director::restart_slot(Engine &engine, uint8_t slot)
{
    if (slot < max_slots && slots_[slot].info.state != SlotState::empty)
    {
        start(engine, slot, slots_[slot].info.spec, Transition::fade);
    }
}

void Director::revert_slot(Engine &engine, uint8_t slot)
{
    if (slot >= max_slots)
    {
        return;
    }
    SlotRuntime &rt = slots_[slot];
    const SlotSpec &spec = library_.base().specs[slot];
    bool live = rt.info.state == SlotState::running || rt.info.state == SlotState::entering;
    if (live && rt.info.spec.same_mode_and_params(spec))
    {
        return;
    }
    rt.has_chain = false;
    rt.info.loops_done = 0;
    start(engine, slot, spec, Transition::fade);
}

void Director::count_cycle(uint8_t slot)
{
    if (slot < max_slots)
    {
        slots_[slot].info.cycles_done++;
    }
}

void Director::begin_ending(Engine &engine, uint8_t slot, EndPolicy policy, uint8_t next)
{
    SlotRuntime &rt = slots_[slot];
    rt.resolved = policy;
    rt.resolved_next = next;
    revision_++;
    if (policy == EndPolicy::hold)
    {
        rt.held = true;
        return;
    }
    rt.info.state = SlotState::ending;
    rt.phase_t = 0.0f;
    Behavior &b = engine.behavior();
    for (uint8_t i = 0; i < max_emitters_per_slot; i++)
    {
        if (Emitter *em = b.emitter(slot, i))
        {
            em->active = false;
        }
    }
    if (!rt.info.spec.life.drain)
    {
        finish_ending(engine, slot);
    }
}

void Director::finish_ending(Engine &engine, uint8_t slot)
{
    SlotRuntime &rt = slots_[slot];
    const Lifecycle life = rt.info.spec.life;
    EndPolicy policy = rt.resolved;
    uint8_t next = rt.resolved_next;
    rt.info.state = SlotState::running;   // until the next thing takes over

    if (policy == EndPolicy::loop)
    {
        rt.info.loops_done++;
        if (life.repeats == 0 || rt.info.loops_done < life.repeats)
        {
            const uint16_t loops = rt.info.loops_done;
            start(engine, slot, rt.info.spec, life.transition);
            rt.info.loops_done = loops;
            return;
        }
        policy = (rt.has_chain || next != no_spec) ? EndPolicy::chain : EndPolicy::revert;
    }
    switch (policy)
    {
    case EndPolicy::chain:
        if (rt.has_chain)
        {
            // start() copies chain_spec before anything can change it.
            rt.has_chain = false;
            rt.info.loops_done = 0;
            start(engine, slot, rt.chain_spec, life.transition);
        }
        else if (next < max_specs)
        {
            rt.info.loops_done = 0;
            start(engine, slot, current_.specs[next], life.transition);
        }
        else
        {
            start(engine, slot, library_.base().specs[slot], life.transition);
        }
        break;
    case EndPolicy::revert:
        rt.info.loops_done = 0;
        start(engine, slot, library_.base().specs[slot], life.transition);
        break;
    case EndPolicy::remove:
        start(engine, slot, SlotSpec{}, life.transition);
        break;
    case EndPolicy::loop:
    case EndPolicy::hold:
        rt.held = true;
        break;
    }
}

void Director::tick(Engine &engine, float dt)
{
    // A scheduled event's length (its own count: it runs even without a clock).
    if (event_running_ >= 0)
    {
        event_left_s_ -= dt;
        if (event_left_s_ <= 0.0f)
        {
            end_event(engine);
        }
    }
    if (show_.active)
    {
        show_.age_s += dt;
        // At the tick nearest the end: summing float ticks drifts.
        if (show_.age_s >= show_.len_s - dt * 0.5f)
        {
            if (next_show_position(engine))
            {
                start_show_entry(engine);
            }
            else
            {
                end_show(engine);
            }
        }
    }

    scene_age_s_ += dt;
    if (current_.duration_s > 0.0f && scene_age_s_ >= current_.duration_s)
    {
        switch (current_.policy)
        {
        case EndPolicy::loop:
        {
            stats_.scene_loops++;
            // Force everything to start over, even slots that are unchanged.
            // (No local copy of the scene: tick() runs deep inside the
            // engine's frame on the tree, where every byte of stack counts.)
            for (SlotRuntime &rt : slots_)
            {
                rt.info.spec.mode = no_mode;
            }
            apply_scene(engine, current_);
            break;
        }
        case EndPolicy::revert:
            apply_scene(engine, library_.base());
            break;
        default:
            current_.duration_s = 0.0f;   // hold: no further scene ending
            break;
        }
    }

    for (uint8_t s = 0; s < max_slots; s++)
    {
        SlotRuntime &rt = slots_[s];
        const ModeDef *def = mode_at(rt.info.spec.mode);
        if (rt.info.state == SlotState::empty || def == nullptr)
        {
            continue;
        }
        if (def->tick != nullptr)
        {
            ModeContext ctx{engine, s, *def, rt.info.spec.params};
            def->tick(ctx, dt);
        }
        rt.info.age_s += dt;
        switch (rt.info.state)
        {
        case SlotState::entering:
            rt.phase_t += dt;
            if (rt.phase_t >= rt.phase_len)
            {
                rt.info.state = SlotState::running;
                revision_++;
                set_opacity(engine, s, 1.0f);
            }
            else
            {
                set_opacity(engine, s, rt.phase_t / rt.phase_len);
            }
            break;
        case SlotState::running:
        {
            if (rt.held)
            {
                break;
            }
            const Lifecycle &life = rt.info.spec.life;
            if (life.duration_s > 0.0f && rt.info.age_s >= life.duration_s)
            {
                end_slot(engine, s, 0, EndReason::duration);
            }
            else if (life.cycles > 0 && rt.info.cycles_done >= life.cycles)
            {
                end_slot(engine, s, 0, EndReason::cycles);
            }
            break;
        }
        case SlotState::ending:
            rt.phase_t += dt;
            if (finishing_entities(engine, s) == 0)
            {
                finish_ending(engine, s);
            }
            else if (rt.phase_t >= rt.info.spec.life.drain_timeout_s)
            {
                stats_.drain_timeouts++;
                finish_ending(engine, s);
            }
            break;
        case SlotState::leaving:
            rt.phase_t += dt;
            if (rt.phase_t >= rt.phase_len)
            {
                const uint16_t loops = rt.info.loops_done;
                setup_now(engine, s, rt.pending_spec, rt.pending_transition);
                rt.info.loops_done = loops;
            }
            else
            {
                set_opacity(engine, s, 1.0f - rt.phase_t / rt.phase_len);
            }
            break;
        case SlotState::empty:
            break;
        }
    }
}

size_t Director::describe_state(char *out, size_t cap) const
{
    if (cap == 0)
    {
        return 0;
    }
    Json j{out, cap};
    j.raw("{\"rev\":%lu,\"scene\":", static_cast<unsigned long>(revision_));
    j.str(current_.name);
    j.raw(",\"slots\":[");
    for (uint8_t s = 0; s < max_slots; s++)
    {
        const SlotRuntime &rt = slots_[s];
        const SlotSpec &spec = rt.info.spec;
        const ModeDef *def = mode_at(spec.mode);
        // Ids are escaped: an effect's holds its name.
        j.raw("%s{\"mode\":", s ? "," : "");
        j.str(rt.info.state == SlotState::empty ? "" : mode_id(spec.mode));
        j.raw(",\"i\":%d,\"state\":\"%s\",\"age\":%ld,\"cycles\":%u,\"loops\":%u,\"params\":[",
              rt.info.state == SlotState::empty ? -1 : (int)spec.mode, state_name(rt.info.state),
              static_cast<long>(rt.info.age_s), (unsigned)rt.info.cycles_done, (unsigned)rt.info.loops_done);
        for (uint8_t p = 0; def != nullptr && rt.info.state != SlotState::empty && p < def->param_count; p++)
        {
            const ParamValue &v = spec.params[p];
            if (def->params[p].type == ParamType::color)
            {
                j.raw("%s\"#%02x%02x%02x\"", p ? "," : "", unit_to_byte(v.c.r), unit_to_byte(v.c.g),
                      unit_to_byte(v.c.b));
            }
            else
            {
                j.raw("%s", p ? "," : "");
                j.num(v.f);
            }
        }
        const Lifecycle &life = spec.life;
        j.raw("],\"life\":{\"duration\":");
        j.num(life.duration_s);
        j.raw(",\"cycles\":%u,\"policy\":\"%s\",\"held\":%s}}", (unsigned)life.cycles, policy_name(life.policy),
              rt.held ? "true" : "false");
    }
    j.raw("],\"base\":[");
    for (uint8_t s = 0; s < max_slots; s++)
    {
        j.raw("%s", s ? "," : "");
        j.str(mode_id(library_.base().specs[s].mode));
    }
    j.raw("],\"show\":");
    if (show_.active)
    {
        const ShowStatus st = show_status();
        j.raw("{\"n\":");
        j.str(st.name);
        j.raw(",\"entry\":");
        j.str(st.preset);
        j.raw(",\"pos\":%u,\"of\":%u,\"round\":%lu,\"left\":%ld}", (unsigned)st.position, (unsigned)st.count,
              static_cast<unsigned long>(st.rounds), static_cast<long>(st.entry_len_s - st.entry_age_s));
    }
    else
    {
        j.raw("null");
    }
    // The draft effect: its name, revision and the slot running it (-1).
    j.raw(",\"fx\":{\"n\":");
    j.str(draft_.name);
    j.raw(",\"rev\":%lu,\"slot\":%d}", static_cast<unsigned long>(draft_revision_), draft_slot());
    // The lights, the timer and a running event.
    j.raw(",\"lights\":%s,\"timer\":%d,\"event\":", lights_on_ ? "true" : "false", timer_level_);
    if (event_running_ >= 0 && event_running_ < library_.schedule().event_count)
    {
        j.raw("{\"n\":");
        j.str(library_.schedule().events[event_running_].name);
        j.raw(",\"i\":%d,\"left\":%ld}", event_running_, static_cast<long>(event_left_s_));
    }
    else
    {
        j.raw("null");
    }
    j.raw("}");
    return j.len;
}

// ---- capturing the live scene ----

void Director::capture_scene(SceneSpec &out, const char *name) const
{
    out = current_;   // keeps the chain targets (specs 4-7) and the scene's own ending
    std::snprintf(out.name, sizeof(out.name), "%s", name);
    for (uint8_t s = 0; s < max_slots; s++)
    {
        const SlotRuntime &rt = slots_[s];
        SlotSpec &spec = out.specs[s];
        if (rt.info.state == SlotState::leaving && rt.pending)
        {
            spec = rt.pending_spec;   // what the slot is changing to
        }
        else if (rt.info.state == SlotState::empty)
        {
            spec = SlotSpec{};
        }
        else
        {
            spec = rt.info.spec;
        }
        // A chain target set over the protocol isn't one of the scene's
        // specs: give it a free chain-target spec so the saved scene chains too.
        if (rt.has_chain && spec.mode != no_mode)
        {
            for (uint8_t k = max_slots; k < max_specs; k++)
            {
                if (out.specs[k].mode == no_mode)
                {
                    out.specs[k] = rt.chain_spec;
                    spec.life.next = k;
                    break;
                }
            }
        }
    }
}

// ---- shows ----

bool Director::play_show(Engine &engine, uint8_t index)
{
    if (index >= library_.show_count() || library_.show(index).count == 0)
    {
        return false;
    }
    show_.show = library_.show(index);
    show_.active = true;
    show_.rounds = 0;
    show_.pos = 0;
    order_show(engine);
    engine.note("show: %s", show_.show.name);
    start_show_entry(engine);
    return show_.active;
}

void Director::stop_show()
{
    if (show_.active)
    {
        show_.active = false;
        revision_++;
    }
}

ShowStatus Director::show_status() const
{
    ShowStatus st;
    if (!show_.active)
    {
        return st;
    }
    st.playing = true;
    st.name = show_.show.name;
    st.preset = show_.show.entries[show_.order[show_.pos]].preset;
    st.position = show_.pos;
    st.count = show_.show.count;
    st.entry_age_s = show_.age_s;
    st.entry_len_s = show_.len_s;
    st.rounds = show_.rounds;
    return st;
}

void Director::order_show(Engine &engine)
{
    const uint8_t n = show_.show.count;
    const uint8_t last = n > 0 ? show_.order[n - 1] : 0;
    for (uint8_t i = 0; i < n; i++)
    {
        show_.order[i] = i;
    }
    if (!show_.show.shuffle || n < 2)
    {
        return;
    }
    for (uint8_t i = static_cast<uint8_t>(n - 1); i > 0; i--)
    {
        const uint8_t k = static_cast<uint8_t>(engine.rng().below(i + 1u));
        const uint8_t t = show_.order[i];
        show_.order[i] = show_.order[k];
        show_.order[k] = t;
    }
    // Never the same entry twice in a row across rounds.
    if (show_.rounds > 0 && show_.order[0] == last)
    {
        show_.order[0] = show_.order[1];
        show_.order[1] = last;
    }
}

bool Director::next_show_position(Engine &engine)
{
    if (++show_.pos < show_.show.count)
    {
        return true;
    }
    if (!show_.show.loop)
    {
        return false;
    }
    show_.rounds++;
    stats_.show_rounds++;
    show_.pos = 0;
    order_show(engine);
    return true;
}

void Director::start_show_entry(Engine &engine)
{
    // Entries whose preset has been deleted are skipped; a show with none
    // left ends.
    for (uint8_t tries = 0; tries <= show_.show.count; tries++)
    {
        const ShowEntry &e = show_.show.entries[show_.order[show_.pos]];
        const uint8_t p = library_.find_preset(e.preset);
        if (p != no_index)
        {
            const SceneSpec &scene = library_.preset(p);
            show_.len_s = e.duration_s > 0      ? static_cast<float>(e.duration_s)
                          : scene.duration_s > 0 ? scene.duration_s
                                                 : static_cast<float>(default_entry_s);
            show_.age_s = 0.0f;
            stats_.show_entries++;
            apply_scene(engine, scene, Transition::fade, false);
            return;
        }
        stats_.show_skips++;
        if (!next_show_position(engine))
        {
            break;
        }
    }
    end_show(engine);
}

void Director::end_show(Engine &engine)
{
    engine.note("show: %s finished", show_.show.name);
    show_.active = false;
    revert_scene(engine);
}

}  // namespace neotree
