#include "neotree/behavior.hpp"

#include <algorithm>
#include <cmath>

#include "neotree/engine.hpp"

namespace neotree {

namespace {

Vec3 add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 scale(Vec3 a, float k) { return {a.x * k, a.y * k, a.z * k}; }
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// A random point in the unit ball (rejection sampling, bounded tries).
Vec3 random_in_ball(Rng &rng)
{
    for (int tries = 0; tries < 8; tries++)
    {
        Vec3 v{rng.range(-1.0f, 1.0f), rng.range(-1.0f, 1.0f), rng.range(-1.0f, 1.0f)};
        if (dot(v, v) <= 1.0f)
        {
            return v;
        }
    }
    return {};
}

Rgb random_hue(Rng &rng) { return hsv(rng.range(0.0f, 360.0f), 1.0f, 1.0f); }

// A random point inside the tree's volume at height z (area-uniform disc).
Vec3 random_at_height(Rng &rng, float z, float outer_radius)
{
    float r = std::sqrt(rng.uniform()) * outer_radius;
    float a = rng.range(0.0f, two_pi);
    return {r * std::cos(a), r * std::sin(a), z};
}

Response mirrored(Response r)
{
    switch (r)
    {
    case Response::destroy_a: return Response::destroy_b;
    case Response::destroy_b: return Response::destroy_a;
    default: return r;
    }
}

bool group_matches(uint8_t filter, uint8_t group) { return filter == any_group || filter == group; }

// The event type a rule's trigger responds to.
EventType event_for(Trigger t)
{
    switch (t)
    {
    case Trigger::collision: return EventType::collision;
    case Trigger::overlap_begin: return EventType::overlap_begin;
    case Trigger::overlap_end: return EventType::overlap_end;
    case Trigger::boundary: return EventType::boundary;
    case Trigger::expired: return EventType::expired;
    case Trigger::spawned: return EventType::spawned;
    case Trigger::timer: return EventType::timer;
    case Trigger::signal: return EventType::signal;
    case Trigger::input: return EventType::input;
    case Trigger::count_below: return EventType::count_below;
    case Trigger::count_above: return EventType::count_above;
    }
    return EventType::signal;
}

// Fills an event with entity data (either may be null).
Event make_event(EventType type, uint8_t slot, const Entity *a, Handle ha, const Entity *b, Handle hb)
{
    Event ev;
    ev.type = type;
    ev.slot = slot;
    ev.a = ha;
    ev.b = hb;
    if (a != nullptr)
    {
        ev.group_a = a->group;
        ev.pos_a = a->pos;
        ev.vel_a = a->vel;
        ev.color_a = a->color;
        ev.point = a->pos;
    }
    if (b != nullptr)
    {
        ev.group_b = b->group;
        ev.pos_b = b->pos;
        ev.vel_b = b->vel;
        ev.color_b = b->color;
        if (a != nullptr)
        {
            ev.point = scale(add(a->pos, b->pos), 0.5f);
        }
    }
    return ev;
}

}  // namespace

// ---- setup ----

void Behavior::clear()
{
    for (uint8_t s = 0; s < max_slots; s++)
    {
        clear_slot(s);
    }
    queue_len_[0] = queue_len_[1] = 0;
    contact_count_ = 0;
    stats_ = {};
}

void Behavior::clear_slot(uint8_t slot)
{
    if (slot >= max_slots)
    {
        return;
    }
    slots_[slot] = SlotBehavior{};
    for (uint16_t i = 0; i < contact_count_;)
    {
        if (contacts_[i].slot == slot)
        {
            contacts_[i] = contacts_[--contact_count_];
        }
        else
        {
            i++;
        }
    }
}

int Behavior::add_template(uint8_t slot, const Entity &entity)
{
    if (slot >= max_slots || slots_[slot].template_count >= max_templates_per_slot)
    {
        return -1;
    }
    SlotBehavior &sb = slots_[slot];
    sb.templates[sb.template_count] = entity;
    sb.templates[sb.template_count].slot = slot;
    return sb.template_count++;
}

Entity *Behavior::template_at(uint8_t slot, uint8_t index)
{
    return slot < max_slots && index < slots_[slot].template_count ? &slots_[slot].templates[index] : nullptr;
}

int Behavior::add_rule(uint8_t slot, const Rule &rule)
{
    if (slot >= max_slots || slots_[slot].rule_count >= max_rules_per_slot)
    {
        return -1;
    }
    SlotBehavior &sb = slots_[slot];
    Rule &r = sb.rules[sb.rule_count];
    r = rule;
    r.fired = 0;
    r.cooldown_left = 0.0f;
    r.timer_left = rule.delay_s > 0.0f ? rule.delay_s : rule.period_s;
    sb.listening |= static_cast<uint16_t>(1u << static_cast<unsigned>(event_for(rule.trigger)));
    // `above` = "the count condition held last tick". A count_below rule
    // starts as if it held, so it has to see the count reach the threshold
    // before it can fire (not on an empty slot at setup).
    r.above = rule.trigger == Trigger::count_below;
    return sb.rule_count++;
}

Rule *Behavior::rule(uint8_t slot, uint8_t index)
{
    return slot < max_slots && index < slots_[slot].rule_count ? &slots_[slot].rules[index] : nullptr;
}

int Behavior::add_emitter(uint8_t slot, const Emitter &emitter)
{
    if (slot >= max_slots || slots_[slot].emitter_count >= max_emitters_per_slot)
    {
        return -1;
    }
    SlotBehavior &sb = slots_[slot];
    sb.emitters[sb.emitter_count] = emitter;
    sb.emitters[sb.emitter_count].accumulator = 0.0f;
    return sb.emitter_count++;
}

Emitter *Behavior::emitter(uint8_t slot, uint8_t index)
{
    return slot < max_slots && index < slots_[slot].emitter_count ? &slots_[slot].emitters[index] : nullptr;
}

void Behavior::set_response(uint8_t slot, uint8_t group_a, uint8_t group_b, Response response)
{
    if (slot >= max_slots || group_a >= max_groups || group_b >= max_groups)
    {
        return;
    }
    slots_[slot].responses[group_a][group_b] = response;
    slots_[slot].responses[group_b][group_a] = mirrored(response);
}

Response Behavior::response(uint8_t slot, uint8_t group_a, uint8_t group_b) const
{
    if (slot >= max_slots || group_a >= max_groups || group_b >= max_groups)
    {
        return Response::ignore;
    }
    return slots_[slot].responses[group_a][group_b];
}

void Behavior::set_quota(uint8_t slot, uint16_t max_entities_in_slot)
{
    if (slot < max_slots)
    {
        slots_[slot].quota = max_entities_in_slot;
    }
}

uint16_t Behavior::count(uint8_t slot, uint8_t group) const
{
    return slot < max_slots && group < max_groups ? slots_[slot].counts[group] : 0;
}

uint16_t Behavior::slot_count(uint8_t slot) const
{
    return slot < max_slots ? slots_[slot].total : 0;
}

// ---- events and rules ----

bool Behavior::listens(uint8_t slot, EventType type) const
{
    const uint16_t bit = static_cast<uint16_t>(1u << static_cast<unsigned>(type));
    if (type == EventType::signal)
    {
        for (const SlotBehavior &sb : slots_)
        {
            if (sb.listening & bit)
            {
                return true;
            }
        }
        return false;
    }
    return slot < max_slots && (slots_[slot].listening & bit) != 0;
}

void Behavior::raise(const Event &event)
{
    if (!listens(event.slot, event.type))
    {
        return;
    }
    uint16_t &len = queue_len_[current_];
    if (len >= max_events)
    {
        stats_.events_dropped++;
        return;
    }
    queues_[current_][len++] = event;
}

bool Behavior::matches(const Rule &rule, const Event &ev, bool &swapped) const
{
    swapped = false;
    switch (rule.trigger)
    {
    case Trigger::collision:
    case Trigger::overlap_begin:
    case Trigger::overlap_end:
    {
        EventType want = rule.trigger == Trigger::collision       ? EventType::collision
                         : rule.trigger == Trigger::overlap_begin ? EventType::overlap_begin
                                                                  : EventType::overlap_end;
        if (ev.type != want)
        {
            return false;
        }
        if (group_matches(rule.group_a, ev.group_a) && group_matches(rule.group_b, ev.group_b))
        {
            return true;
        }
        if (group_matches(rule.group_a, ev.group_b) && group_matches(rule.group_b, ev.group_a))
        {
            swapped = true;
            return true;
        }
        return false;
    }
    case Trigger::boundary:
        return ev.type == EventType::boundary && group_matches(rule.group_a, ev.group_a) &&
               (rule.id == 0 || (rule.id & ev.id) != 0);
    case Trigger::expired:
        return ev.type == EventType::expired && group_matches(rule.group_a, ev.group_a);
    case Trigger::spawned:
        return ev.type == EventType::spawned && group_matches(rule.group_a, ev.group_a);
    case Trigger::signal:
        return ev.type == EventType::signal && rule.id == ev.id;
    case Trigger::input:
        return ev.type == EventType::input && rule.id == ev.id;
    default:
        return false;   // timers and counts aren't event-driven
    }
}

void Behavior::fire(Engine &engine, uint8_t slot, Rule &rule, const Event *event, bool swapped)
{
    if (!rule.enabled || rule.cooldown_left > 0.0f || (rule.max_fires != 0 && rule.fired >= rule.max_fires))
    {
        return;
    }
    if (rule.probability < 1.0f && engine.rng().uniform() >= rule.probability)
    {
        return;
    }
    if (rule.limit_group < max_groups && count(slot, rule.limit_group) >= rule.limit_count)
    {
        return;
    }
    rule.fired++;
    rule.cooldown_left = rule.cooldown_s;
    stats_.rule_fires++;
    for (uint8_t i = 0; i < rule.action_count; i++)
    {
        if (actions_left_ == 0)
        {
            stats_.actions_dropped++;
            continue;
        }
        actions_left_--;
        run_action(engine, slot, rule.actions[i], event, swapped);
    }
}

void Behavior::handle_events(Engine &engine, float dt)
{
    actions_left_ = actions_per_tick;

    // Last tick's events; anything raised while handling them waits a tick.
    const uint8_t handling = current_;
    current_ ^= 1;
    queue_len_[current_] = 0;

    for (uint8_t s = 0; s < max_slots; s++)
    {
        for (uint8_t r = 0; r < slots_[s].rule_count; r++)
        {
            Rule &rule = slots_[s].rules[r];
            rule.cooldown_left = std::max(0.0f, rule.cooldown_left - dt);
        }
    }

    for (uint16_t e = 0; e < queue_len_[handling]; e++)
    {
        const Event &ev = queues_[handling][e];
        stats_.events++;
        // Signals reach every slot; everything else stays in its own.
        uint8_t first = ev.type == EventType::signal ? 0 : ev.slot;
        uint8_t last = ev.type == EventType::signal ? max_slots - 1 : ev.slot;
        for (uint8_t s = first; s <= last && s < max_slots; s++)
        {
            for (uint8_t r = 0; r < slots_[s].rule_count; r++)
            {
                bool swapped = false;
                if (matches(slots_[s].rules[r], ev, swapped))
                {
                    fire(engine, s, slots_[s].rules[r], &ev, swapped);
                }
            }
        }
    }

    // Timers and count thresholds.
    for (uint8_t s = 0; s < max_slots; s++)
    {
        for (uint8_t r = 0; r < slots_[s].rule_count; r++)
        {
            Rule &rule = slots_[s].rules[r];
            if (!rule.enabled)
            {
                continue;
            }
            if (rule.trigger == Trigger::timer)
            {
                rule.timer_left -= dt;
                // A hair of tolerance: repeated float subtraction of dt can
                // leave a tiny positive remainder where it should be zero.
                if (rule.timer_left <= dt * 1e-3f)
                {
                    Event tick_event;
                    tick_event.type = EventType::timer;
                    tick_event.slot = s;
                    fire(engine, s, rule, &tick_event, false);
                    if (rule.period_s > 0.0f)
                    {
                        rule.timer_left += rule.period_s;
                    }
                    else
                    {
                        rule.enabled = false;   // one-shot
                    }
                }
            }
            else if (rule.trigger == Trigger::count_below || rule.trigger == Trigger::count_above)
            {
                uint16_t c = count(s, rule.group_a);
                bool now = rule.trigger == Trigger::count_above ? c > rule.threshold : c < rule.threshold;
                if (now && !rule.above)
                {
                    Event count_event;
                    count_event.type =
                        rule.trigger == Trigger::count_above ? EventType::count_above : EventType::count_below;
                    count_event.slot = s;
                    count_event.group_a = rule.group_a;
                    count_event.value = c;
                    fire(engine, s, rule, &count_event, false);
                }
                rule.above = now;
            }
        }
    }
}

void Behavior::run_action(Engine &engine, uint8_t slot, const Action &act, const Event *ev, bool swapped)
{
    // The event's two sides, as the rule sees them (a = its group_a side).
    Event none;
    const Event &e = ev != nullptr ? *ev : none;
    const Handle ha = swapped ? e.b : e.a;
    const Handle hb = swapped ? e.a : e.b;
    const Vec3 pos_a = swapped ? e.pos_b : e.pos_a;
    const Vec3 pos_b = swapped ? e.pos_a : e.pos_b;
    const Vec3 vel_a = swapped ? e.vel_b : e.vel_a;
    const Rgb col_a = swapped ? e.color_b : e.color_a;
    const Rgb col_b = swapped ? e.color_a : e.color_b;
    Rng &rng = engine.rng();

    const Rgb burst = random_hue(rng);   // shared by everything this action colors
    auto color_for = [&](Rgb current) -> Rgb {
        switch (act.color_from)
        {
        case ColorFrom::keep: return current;
        case ColorFrom::fixed: return act.color;
        case ColorFrom::a: return col_a;
        case ColorFrom::b: return col_b;
        case ColorFrom::mix: return lerp(col_a, col_b, 0.5f);
        case ColorFrom::random_hue: return burst;
        case ColorFrom::random_each: return random_hue(rng);
        }
        return current;
    };

    // Applies fn to the action's target entities.
    auto for_targets = [&](auto fn) {
        auto one = [&](Handle h) {
            if (Entity *t = engine.entity(h))
            {
                fn(*t, h);
            }
        };
        switch (act.target)
        {
        case Target::a: one(ha); break;
        case Target::b: one(hb); break;
        case Target::both: one(ha); one(hb); break;
        case Target::group:
        {
            const EntityPool &pool = engine.entities();
            for (uint16_t k = 0; k < pool.capacity; k++)
            {
                if (pool.alive(k) && pool.item(k).slot == slot && pool.item(k).group == act.group)
                {
                    one(pool.handle_at(k));
                }
            }
            break;
        }
        }
    };

    switch (act.type)
    {
    case ActionType::spawn:
    {
        const Entity *tmpl = template_at(slot, act.index);
        if (tmpl == nullptr)
        {
            return;
        }
        for (uint8_t k = 0; k < act.count; k++)
        {
            Entity n = *tmpl;
            switch (act.place)
            {
            case Place::event_point: n.pos = e.point; break;
            case Place::a: n.pos = pos_a; break;
            case Place::b: n.pos = pos_b; break;
            case Place::fixed: n.pos = act.pos; break;
            case Place::anywhere:
            {
                const World &w = engine.world();
                float z = rng.range(w.floor_z, w.ceiling_z);
                n.pos = random_at_height(rng, z, engine.geometry().envelope_radius(z));
                break;
            }
            }
            n.vel = add(add(n.vel, act.vel), scale(vel_a, act.inherit));
            if (act.spread > 0.0f)
            {
                n.vel = add(n.vel, scale(random_in_ball(rng), act.spread));
            }
            n.color = color_for(n.color);
            spawn(engine, slot, n);
        }
        break;
    }
    case ActionType::destroy:
    {
        // Collect first: destroying while walking the pool is fine, but a
        // target referenced twice (both, a == b) must only go once.
        for_targets([&](Entity &, Handle h) { engine.destroy(h); });
        break;
    }
    case ActionType::set_color:
        for_targets([&](Entity &t, Handle) { t.color = color_for(t.color); });
        break;
    case ActionType::set_velocity:
        for_targets([&](Entity &t, Handle) { t.vel = act.vel; });
        break;
    case ActionType::impulse:
        for_targets([&](Entity &t, Handle) { t.vel = add(t.vel, act.vel); });
        break;
    case ActionType::emitter_on:
    case ActionType::emitter_off:
    case ActionType::emitter_rate:
        if (Emitter *em = emitter(slot, act.index))
        {
            if (act.type == ActionType::emitter_rate)
            {
                em->rate = act.value;
            }
            else
            {
                em->active = act.type == ActionType::emitter_on;
            }
        }
        break;
    case ActionType::set_gravity:
        engine.forces().gravity = act.vel;
        break;
    case ActionType::set_wind:
        engine.forces().wind = act.vel;
        break;
    case ActionType::layer_color:
        if (Layer *layer = engine.scene().layer(slot, act.index))
        {
            layer->color = color_for(layer->color);
        }
        break;
    case ActionType::layer_opacity:
        if (Layer *layer = engine.scene().layer(slot, act.index))
        {
            layer->opacity = act.value;
        }
        break;
    case ActionType::cycle:
        engine.director().count_cycle(slot);
        break;
    case ActionType::end_mode:
        engine.director().end_slot(engine, slot, act.index, EndReason::outcome);
        break;
    case ActionType::signal:
    {
        Event sig;
        sig.type = EventType::signal;
        sig.slot = slot;
        sig.id = act.index;
        raise(sig);
        break;
    }
    }
}

Handle Behavior::spawn(Engine &engine, uint8_t slot, const Entity &entity)
{
    if (slot >= max_slots)
    {
        return {};
    }
    SlotBehavior &sb = slots_[slot];
    if (sb.total >= sb.quota)
    {
        stats_.spawns_over_quota++;
        return {};
    }
    Entity e = entity;
    e.slot = slot;
    Handle h = engine.spawn(e);
    if (!h.valid())
    {
        return h;
    }
    sb.total++;
    if (e.group < max_groups)
    {
        sb.counts[e.group]++;
    }
    raise(make_event(EventType::spawned, slot, &e, h, nullptr, {}));
    return h;
}

// ---- emitters ----

void Behavior::run_emitters(Engine &engine, float dt)
{
    Rng &rng = engine.rng();
    const World &w = engine.world();
    const LedGeometry &g = engine.geometry();
    for (uint8_t s = 0; s < max_slots; s++)
    {
        SlotBehavior &sb = slots_[s];
        for (uint8_t i = 0; i < sb.emitter_count; i++)
        {
            Emitter &em = sb.emitters[i];
            if (!em.active || em.template_index >= sb.template_count)
            {
                continue;
            }
            em.accumulator += em.rate * dt;
            for (int guard = 0; em.accumulator >= 1.0f && guard < 16; guard++)
            {
                em.accumulator -= 1.0f;
                Entity n = sb.templates[em.template_index];
                switch (em.region)
                {
                case Region::point: n.pos = em.min; break;
                case Region::box:
                    n.pos = {rng.range(em.min.x, em.max.x), rng.range(em.min.y, em.max.y),
                             rng.range(em.min.z, em.max.z)};
                    break;
                case Region::band:
                {
                    float r = std::sqrt(rng.range(em.min.x * em.min.x, em.max.x * em.max.x));
                    float a = rng.range(0.0f, two_pi);
                    n.pos = {r * std::cos(a), r * std::sin(a), rng.range(em.min.z, em.max.z)};
                    break;
                }
                case Region::top:
                    n.pos = random_at_height(rng, w.ceiling_z, g.envelope_radius(w.ceiling_z));
                    break;
                case Region::volume:
                {
                    float z = rng.range(w.floor_z, w.ceiling_z);
                    n.pos = random_at_height(rng, z, g.envelope_radius(z));
                    break;
                }
                case Region::entity:
                {
                    const Entity *host = engine.entity(em.attach);
                    if (host == nullptr)
                    {
                        em.active = false;   // what it rode on is gone
                        em.accumulator = 0.0f;
                        continue;
                    }
                    n.pos = add(host->pos, em.min);
                    break;
                }
                }
                n.vel = add(n.vel, em.vel);
                if (em.spread > 0.0f)
                {
                    n.vel = add(n.vel, scale(random_in_ball(rng), em.spread));
                }
                if (em.size_jitter > 0.0f)
                {
                    n.size *= 1.0f + rng.range(-em.size_jitter, em.size_jitter);
                }
                if (em.lifetime_jitter > 0.0f)
                {
                    n.lifetime_s *= 1.0f + rng.range(-em.lifetime_jitter, em.lifetime_jitter);
                }
                if (em.color_from == ColorFrom::fixed)
                {
                    n.color = em.color;
                }
                else if (em.color_from == ColorFrom::random_each || em.color_from == ColorFrom::random_hue)
                {
                    n.color = random_hue(rng);
                }
                spawn(engine, s, n);
            }
        }
    }
}

// ---- collisions ----

Behavior::Contact *Behavior::find_contact(Handle a, Handle b)
{
    for (uint16_t i = 0; i < contact_count_; i++)
    {
        Contact &c = contacts_[i];
        if ((c.a == a && c.b == b) || (c.a == b && c.b == a))
        {
            return &c;
        }
    }
    return nullptr;
}

void Behavior::collide(Engine &engine)
{
    const EntityPool &pool = engine.entities();
    // Static: 256-entry work arrays are too big for the Pico's stack.
    static uint16_t idx[max_entities];
    static float zlo[max_entities];

    for (uint8_t s = 0; s < max_slots; s++)
    {
        const SlotBehavior &sb = slots_[s];
        bool any = false;
        for (uint8_t i = 0; i < max_groups && !any; i++)
        {
            for (uint8_t j = 0; j < max_groups && !any; j++)
            {
                any = sb.responses[i][j] != Response::ignore;
            }
        }
        if (!any)
        {
            continue;
        }

        // Colliders in this slot, sorted by the bottom of their sphere.
        uint16_t n = 0;
        for (uint16_t k = 0; k < pool.capacity; k++)
        {
            if (!pool.alive(k))
            {
                continue;
            }
            const Entity &e = pool.item(k);
            if (e.slot != s || e.group >= max_groups)
            {
                continue;
            }
            float r = collision_radius(e);
            if (r <= 0.0f)
            {
                continue;
            }
            uint16_t at = n++;
            float lo = e.pos.z - r;
            while (at > 0 && zlo[at - 1] > lo)   // insertion sort
            {
                idx[at] = idx[at - 1];
                zlo[at] = zlo[at - 1];
                at--;
            }
            idx[at] = k;
            zlo[at] = lo;
        }

        for (uint16_t i = 0; i < n; i++)
        {
            Handle ha = pool.handle_at(idx[i]);
            Entity *a = engine.entity(ha);
            if (a == nullptr)
            {
                continue;   // destroyed by an earlier contact this tick
            }
            const float ra = collision_radius(*a);
            const float a_top = a->pos.z + ra;
            for (uint16_t j = i + 1; j < n && zlo[j] <= a_top; j++)
            {
                Handle hb = pool.handle_at(idx[j]);
                Entity *b = engine.entity(hb);
                if (b == nullptr || (a = engine.entity(ha)) == nullptr)
                {
                    break;
                }
                Response resp = sb.responses[a->group][b->group];
                if (resp == Response::ignore)
                {
                    continue;
                }
                const float rb = collision_radius(*b);
                Vec3 d = sub(b->pos, a->pos);
                float dist2 = dot(d, d);
                float reach = ra + rb;
                if (dist2 >= reach * reach)
                {
                    continue;
                }

                Contact *c = find_contact(ha, hb);
                bool is_new = c == nullptr;
                if (is_new)
                {
                    if (contact_count_ < max_contacts)
                    {
                        c = &contacts_[contact_count_++];
                        *c = Contact{ha, hb, s, true};
                    }
                    else
                    {
                        stats_.contacts_dropped++;
                    }
                }
                else
                {
                    c->seen = true;
                }

                float dist = std::sqrt(dist2);
                Vec3 nrm = dist > 1e-3f ? scale(d, 1.0f / dist) : Vec3{0.0f, 0.0f, 1.0f};
                float closing = -dot(sub(b->vel, a->vel), nrm);   // > 0: approaching

                switch (resp)
                {
                case Response::overlap:
                    if (is_new)
                    {
                        raise(make_event(EventType::overlap_begin, s, a, ha, b, hb));
                    }
                    break;
                case Response::bounce:
                {
                    float inv_a = a->kinematic ? 0.0f : 1.0f / std::max(a->mass, 1e-3f);
                    float inv_b = b->kinematic ? 0.0f : 1.0f / std::max(b->mass, 1e-3f);
                    float inv = inv_a + inv_b;
                    if (inv > 0.0f)
                    {
                        if (closing > 0.0f)
                        {
                            float rest = std::min(a->restitution, b->restitution);
                            float impulse = (1.0f + rest) * closing / inv;
                            a->vel = sub(a->vel, scale(nrm, impulse * inv_a));
                            b->vel = add(b->vel, scale(nrm, impulse * inv_b));
                        }
                        // Separate so they don't stay interlocked.
                        float push = (reach - dist) / inv;
                        a->pos = sub(a->pos, scale(nrm, push * inv_a));
                        b->pos = add(b->pos, scale(nrm, push * inv_b));
                    }
                    if (is_new)
                    {
                        Event ev = make_event(EventType::collision, s, a, ha, b, hb);
                        ev.value = closing;
                        raise(ev);
                    }
                    break;
                }
                case Response::stick:
                {
                    Vec3 v;
                    if (a->kinematic || b->kinematic)
                    {
                        v = a->kinematic ? a->vel : b->vel;
                    }
                    else
                    {
                        float ma = std::max(a->mass, 1e-3f), mb = std::max(b->mass, 1e-3f);
                        v = scale(add(scale(a->vel, ma), scale(b->vel, mb)), 1.0f / (ma + mb));
                    }
                    a->vel = v;
                    b->vel = v;
                    if (is_new)
                    {
                        raise(make_event(EventType::collision, s, a, ha, b, hb));
                    }
                    break;
                }
                case Response::destroy_a:
                case Response::destroy_b:
                case Response::destroy_both:
                {
                    Event ev = make_event(EventType::collision, s, a, ha, b, hb);
                    ev.value = closing;
                    raise(ev);
                    if (resp != Response::destroy_b)
                    {
                        engine.destroy(ha);
                    }
                    if (resp != Response::destroy_a)
                    {
                        engine.destroy(hb);
                    }
                    break;
                }
                case Response::ignore:
                    break;
                }
                if (engine.entity(ha) == nullptr)
                {
                    break;   // a is gone: no more pairs for it
                }
            }
        }
    }

    // Contacts that ended: overlap end events, then forget them.
    for (uint16_t i = 0; i < contact_count_;)
    {
        Contact &c = contacts_[i];
        if (c.seen)
        {
            c.seen = false;
            i++;
            continue;
        }
        const Entity *a = engine.entity(c.a);
        const Entity *b = engine.entity(c.b);
        if (a != nullptr && b != nullptr && response(c.slot, a->group, b->group) == Response::overlap)
        {
            raise(make_event(EventType::overlap_end, c.slot, a, c.a, b, c.b));
        }
        c = contacts_[--contact_count_];
    }
}

void Behavior::recount(const EntityPool &pool)
{
    for (SlotBehavior &sb : slots_)
    {
        sb.total = 0;
        for (uint16_t &c : sb.counts)
        {
            c = 0;
        }
    }
    for (uint16_t k = 0; k < pool.capacity; k++)
    {
        if (!pool.alive(k))
        {
            continue;
        }
        const Entity &e = pool.item(k);
        if (e.slot >= max_slots)
        {
            continue;
        }
        slots_[e.slot].total++;
        if (e.group < max_groups)
        {
            slots_[e.slot].counts[e.group]++;
        }
    }
    stats_.peak_entities = std::max(stats_.peak_entities, pool.size());
}

}  // namespace neotree
