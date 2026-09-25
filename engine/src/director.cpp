#include "neotree/director.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "neotree/engine.hpp"

namespace neotree {

namespace {

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

struct Json
{
    char *out;
    size_t cap;
    size_t len = 0;

    void raw(const char *fmt, ...)
    {
        if (len + 1 >= cap)
        {
            return;
        }
        va_list args;
        va_start(args, fmt);
        int n = vsnprintf(out + len, cap - len, fmt, args);
        va_end(args);
        if (n > 0)
        {
            len = len + static_cast<size_t>(n) < cap ? len + static_cast<size_t>(n) : cap - 1;
        }
    }
    // Numbers the same on every printf: integers plainly, otherwise up to
    // three decimals with trailing zeros trimmed (the Pico's %g pads).
    void num(double v)
    {
        char buf[24];
        long whole = static_cast<long>(v);
        if (static_cast<double>(whole) == v)
        {
            std::snprintf(buf, sizeof(buf), "%ld", whole);
        }
        else
        {
            std::snprintf(buf, sizeof(buf), "%.3f", v);
            char *end = buf + std::strlen(buf) - 1;
            while (end > buf && *end == '0')
            {
                *end-- = '\0';
            }
        }
        raw("%s", buf);
    }
};

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
    base_ = SceneSpec{};
    current_ = SceneSpec{};
    scene_age_s_ = 0.0f;
    stats_ = {};
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

void Director::apply_scene(Engine &engine, const SceneSpec &scene, Transition transition)
{
    current_ = scene;
    scene_age_s_ = 0.0f;
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
    ModeContext ctx{engine, slot, *def, rt.info.spec.params};
    if (def->on_param != nullptr && def->on_param(ctx, index))
    {
        return true;
    }
    // Set it up again with the new value, keeping its place in its lifecycle.
    const SlotInfo keep = rt.info;
    const float opacity_k = engine.scene().slot(slot)->opacity;
    setup_now(engine, slot, rt.info.spec, Transition::cut);
    rt.info.age_s = keep.age_s;
    rt.info.cycles_done = keep.cycles_done;
    rt.info.loops_done = keep.loops_done;
    rt.info.state = keep.state;
    engine.scene().slot(slot)->opacity = opacity_k;
    return true;
}

void Director::set_lifecycle(uint8_t slot, const Lifecycle &life, const SlotSpec *chain_to)
{
    if (slot >= max_slots)
    {
        return;
    }
    SlotRuntime &rt = slots_[slot];
    rt.info.spec.life = life;
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
    const SlotSpec &spec = base_.specs[slot];
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
            start(engine, slot, base_.specs[slot], life.transition);
        }
        break;
    case EndPolicy::revert:
        rt.info.loops_done = 0;
        start(engine, slot, base_.specs[slot], life.transition);
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
            apply_scene(engine, base_);
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
    j.raw("{\"scene\":\"%s\",\"slots\":[", current_.name);
    for (uint8_t s = 0; s < max_slots; s++)
    {
        const SlotRuntime &rt = slots_[s];
        const SlotSpec &spec = rt.info.spec;
        const ModeDef *def = mode_at(spec.mode);
        j.raw("%s{\"mode\":\"%s\",\"i\":%d,\"state\":\"%s\",\"age\":%ld,\"cycles\":%u,\"loops\":%u,\"params\":[",
              s ? "," : "", rt.info.state == SlotState::empty ? "" : mode_id(spec.mode),
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
        j.raw("%s\"%s\"", s ? "," : "", mode_id(base_.specs[s].mode));
    }
    j.raw("]}");
    return j.len;
}

// ---- presets ----

namespace {

constexpr uint8_t count_of_presets = 5;
SceneSpec presets[count_of_presets];
bool presets_built = false;

void name_scene(SceneSpec &scene, const char *name)
{
    std::snprintf(scene.name, sizeof(scene.name), "%s", name);
}

void build_presets()
{
    if (presets_built)
    {
        return;
    }
    presets_built = true;

    // 0: today's manual colors - the default base scene.
    SceneSpec &colors = presets[0];
    name_scene(colors, "Colors");
    colors.specs[0] = SlotSpec::of("canvas");

    // 1: two modes stacked - snow (no backdrop) over a slow rainbow.
    SceneSpec &snowbow = presets[1];
    name_scene(snowbow, "Snow on rainbow");
    snowbow.specs[0] = SlotSpec::of("rainbow").set("speed", 0.08f).set("brightness", 0.45f);
    snowbow.specs[1] = SlotSpec::of("snow").set("backdrop", 0.0f);

    // 2: a show - fireworks for 30 s, then snow for 45 s, then fireworks
    // again, forever, over a night-sky gradient.
    SceneSpec &show = presets[2];
    name_scene(show, "Holiday show");
    show.specs[0] = SlotSpec::of("gradient").set("bottom", Rgb{0.0f, 0.02f, 0.12f}).set("top", Rgb{0.08f, 0.0f, 0.1f});
    show.specs[1] = SlotSpec::of("fireworks").set("backdrop", 0.0f);
    show.specs[1].life.duration_s = 30.0f;
    show.specs[1].life.policy = EndPolicy::chain;
    show.specs[1].life.next = 4;
    show.specs[4] = SlotSpec::of("snow").set("backdrop", 0.0f);
    show.specs[4].life.duration_s = 45.0f;
    show.specs[4].life.policy = EndPolicy::chain;
    show.specs[4].life.next = 1;
    show.specs[4].life.drain_timeout_s = 14.0f;   // flakes live 12 s: let them all settle and fade

    // 3: the Pi's three sweeps in turn, by cycles: 5 linear passes, 3
    // drops, 3 launches, repeat.
    SceneSpec &sweeps = presets[3];
    name_scene(sweeps, "Sweep tour");
    sweeps.specs[0] = SlotSpec::of("solid").set("color", to_rgb(0, 140, 0));
    sweeps.specs[1] = SlotSpec::of("sweep").set("motion", 0.0f).set("backdrop", 0.0f);
    sweeps.specs[1].life.cycles = 5;
    sweeps.specs[1].life.policy = EndPolicy::chain;
    sweeps.specs[1].life.next = 4;
    sweeps.specs[4] = SlotSpec::of("sweep").set("motion", 1.0f).set("backdrop", 0.0f);
    sweeps.specs[4].life.cycles = 3;
    sweeps.specs[4].life.policy = EndPolicy::chain;
    sweeps.specs[4].life.next = 5;
    sweeps.specs[5] = SlotSpec::of("sweep").set("motion", 2.0f).set("backdrop", 0.0f);
    sweeps.specs[5].life.cycles = 3;
    sweeps.specs[5].life.policy = EndPolicy::chain;
    sweeps.specs[5].life.next = 1;

    // 4: a dozen fireworks over the family's colors, then back to just the
    // colors (revert: the base scene has nothing in slot 1).
    SceneSpec &finale = presets[4];
    name_scene(finale, "Fireworks finale");
    finale.specs[0] = SlotSpec::of("canvas");
    finale.specs[1] = SlotSpec::of("fireworks").set("backdrop", 0.0f).set("rate", 1.5f);
    finale.specs[1].life.cycles = 12;
    finale.specs[1].life.policy = EndPolicy::revert;
}

}  // namespace

uint8_t preset_count()
{
    build_presets();
    return count_of_presets;
}

const SceneSpec &preset_at(uint8_t index)
{
    build_presets();
    return presets[index < count_of_presets ? index : 0];
}

uint8_t find_preset(const char *name)
{
    build_presets();
    for (uint8_t i = 0; i < count_of_presets; i++)
    {
        if (std::strcmp(presets[i].name, name) == 0)
        {
            return i;
        }
    }
    return 0xFF;
}

}  // namespace neotree
