#include "neotree/engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace neotree {

void Engine::init(const LedGeometry &geometry, const EngineConfig &config, LogFn log)
{
    geometry_ = &geometry;
    config_ = config;
    log_ = log;
    clock_ = SimClock(config.tick_hz);
    rng_.reseed(config.seed);
    stats_ = {};
    scene_.clear();
    master_ = {};
    entities_.clear();
    behavior_.clear();
    forces_ = {};
    world_ = {};
    world_.floor_z = geometry.bounds().min.z;
    world_.ceiling_z = geometry.bounds().max.z;
    this->log("engine: %u LEDs, %u positioned (%u mapped), tick %u Hz, seed %u", (unsigned)geometry.count(),
              (unsigned)geometry.positioned_count(), (unsigned)geometry.mapped_count(), (unsigned)config.tick_hz,
              (unsigned)config.seed);
}

uint32_t Engine::advance(int64_t dt_us)
{
    uint32_t due = clock_.accumulate(dt_us);
    uint32_t run = due;
    if (config_.max_ticks_per_advance != 0 && run > config_.max_ticks_per_advance)
    {
        run = config_.max_ticks_per_advance;
        stats_.ticks_dropped += due - run;
    }
    for (uint32_t i = 0; i < run; i++)
    {
        tick();
    }
    return run;
}

void Engine::run_ticks(uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
    {
        tick();
    }
}

void Engine::tick()
{
    // Rules first (last tick's events, timers, counts), then emitters,
    // motion, collisions. Lifecycles join in M5.
    const float dt = clock_.tick_dt_s();
    behavior_.handle_events(*this, dt);
    behavior_.run_emitters(*this, dt);
    for (uint16_t k = 0; k < entities_.capacity; k++)
    {
        if (!entities_.alive(k))
        {
            continue;
        }
        Entity &e = entities_.item(k);
        const StepResult r = step_entity(e, forces_, world_, *geometry_, dt);
        const Handle h = entities_.handle_at(k);
        if (r.hits != 0 && behavior_.listens(e.slot, EventType::boundary))
        {
            Event ev;
            ev.type = EventType::boundary;
            ev.slot = e.slot;
            ev.id = r.hits;
            ev.group_a = e.group;
            ev.a = h;
            ev.pos_a = ev.point = e.pos;
            ev.vel_a = e.vel;
            ev.color_a = e.color;
            behavior_.raise(ev);
        }
        if (!r.alive)
        {
            if (r.expired && behavior_.listens(e.slot, EventType::expired))
            {
                Event ev;
                ev.type = EventType::expired;
                ev.slot = e.slot;
                ev.group_a = e.group;
                ev.a = h;
                ev.pos_a = ev.point = e.pos;
                ev.vel_a = e.vel;
                ev.color_a = e.color;
                behavior_.raise(ev);
            }
            entities_.destroy(h);
        }
    }
    behavior_.collide(*this);
    behavior_.recount(entities_);
    stats_.entities = entities_.size();
    clock_.advance_tick();
    stats_.ticks++;
}

void Engine::render(std::span<Rgb> out)
{
    size_t n = out.size() < geometry_->count() ? out.size() : geometry_->count();
    std::span<Rgb> leds = out.first(n);
    stats_.frames++;
    if (!master_.output_enabled)
    {
        for (Rgb &px : leds)
        {
            px = Rgb{};
        }
        stats_.last_frame_led_evals = 0;
        return;
    }
    stats_.last_frame_led_evals = composite(scene_, *geometry_, clock_.time_us(), entities_, scratch_, leds);

    const float k = master_.brightness;
    const bool curve = master_.gamma != 1.0f;
    auto finish = [&](float v) {
        v = std::clamp(v * k, 0.0f, 1.0f);
        return curve ? std::pow(v, master_.gamma) : v;
    };
    for (Rgb &px : leds)
    {
        px = {finish(px.r), finish(px.g), finish(px.b)};
    }
}

Handle Engine::spawn(const Entity &entity)
{
    Handle h = entities_.create();
    Entity *e = entities_.get(h);
    if (e == nullptr)
    {
        stats_.spawns_failed++;
        return {};
    }
    *e = entity;
    e->spawn_pos = entity.pos;
    e->spawn_vel = entity.vel;
    e->age_s = 0.0f;
    stats_.entities = entities_.size();
    return h;
}

void Engine::input(uint8_t slot, uint8_t id, float value)
{
    Event ev;
    ev.type = EventType::input;
    ev.slot = slot;
    ev.id = id;
    ev.value = value;
    behavior_.raise(ev);
}

void Engine::destroy_entities_in_slot(int slot)
{
    for (uint16_t k = 0; k < entities_.capacity; k++)
    {
        if (entities_.alive(k) && (slot < 0 || entities_.item(k).slot == slot))
        {
            entities_.destroy(entities_.handle_at(k));
        }
    }
    stats_.entities = entities_.size();
}

void Engine::render_bytes(std::span<Rgb8> out)
{
    size_t n = out.size() < geometry_->count() ? out.size() : geometry_->count();
    stats_.frames++;
    if (!master_.output_enabled)
    {
        for (Rgb8 &px : out.first(n))
        {
            px = Rgb8{};
        }
        stats_.last_frame_led_evals = 0;
        return;
    }
    std::span<Rgb> frame(frame_, n);
    stats_.last_frame_led_evals = composite(scene_, *geometry_, clock_.time_us(), entities_, scratch_, frame);

    const float k = master_.brightness;
    if (master_.gamma == 1.0f)
    {
        for (size_t i = 0; i < n; i++)
        {
            out[i] = {unit_to_byte(frame_[i].r * k), unit_to_byte(frame_[i].g * k), unit_to_byte(frame_[i].b * k)};
        }
        return;
    }
    const float gamma = master_.gamma;
    auto curve = [&](float v) { return unit_to_byte(std::pow(std::clamp(v * k, 0.0f, 1.0f), gamma)); };
    for (size_t i = 0; i < n; i++)
    {
        out[i] = {curve(frame_[i].r), curve(frame_[i].g), curve(frame_[i].b)};
    }
}

void Engine::log(const char *fmt, ...) const
{
    if (log_ == nullptr)
    {
        return;
    }
    char text[128];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    log_(text);
}

}  // namespace neotree
