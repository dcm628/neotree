#include "neotree/engine.hpp"

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
    // Scene edits, forces, motion, collisions, rules, and lifecycles will run
    // here (M2-M5).
    clock_.advance_tick();
    stats_.ticks++;
}

void Engine::render(std::span<Rgb> out)
{
    // The scene is empty until M2: everything renders black.
    size_t n = out.size() < geometry_->count() ? out.size() : geometry_->count();
    for (size_t i = 0; i < n; i++)
    {
        out[i] = Rgb{};
    }
    stats_.last_frame_led_evals = 0;
    stats_.frames++;
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
