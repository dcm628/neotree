#include <vector>

#include "doctest/doctest.h"
#include "neotree/engine.hpp"

using namespace neotree;

namespace {

LedGeometry geometry;
Engine engine;

void setup(const EngineConfig &config)
{
    geometry.reset(10);
    for (uint16_t i = 0; i < 10; i++)
    {
        geometry.set(i, led_point_from_cylindrical(100.0f * i, 200.0f, 36.0f * i));
    }
    geometry.finalize();
    engine.init(geometry, config);
}

}  // namespace

TEST_CASE("engine: advance runs ticks as simulation time passes")
{
    EngineConfig config;
    config.tick_hz = 100;
    config.max_ticks_per_advance = 0;
    setup(config);
    CHECK(engine.advance(1'000'000) == 100);
    CHECK(engine.time_us() == 1'000'000);
    CHECK(engine.stats().ticks == 100);
    CHECK(engine.stats().ticks_dropped == 0);
}

TEST_CASE("engine: max_ticks_per_advance drops (and counts) excess time")
{
    EngineConfig config;
    config.tick_hz = 100;
    config.max_ticks_per_advance = 8;
    setup(config);
    CHECK(engine.advance(1'000'000) == 8);
    CHECK(engine.stats().ticks == 8);
    CHECK(engine.stats().ticks_dropped == 92);
}

TEST_CASE("engine: run_ticks steps exactly")
{
    EngineConfig config;
    config.tick_hz = 120;
    setup(config);
    engine.run_ticks(3);
    CHECK(engine.stats().ticks == 3);
    CHECK(engine.time_us() == 25'000);
}

TEST_CASE("engine: empty scene renders black and counts frames")
{
    setup({});
    std::vector<Rgb> frame(10, Rgb{1.0f, 1.0f, 1.0f});
    engine.render(frame);
    for (const Rgb &px : frame)
    {
        CHECK(px.r == 0.0f);
        CHECK(px.g == 0.0f);
        CHECK(px.b == 0.0f);
    }
    CHECK(engine.stats().frames == 1);
}

TEST_CASE("engine: render never writes past the LED count")
{
    setup({});
    std::vector<Rgb> frame(12, Rgb{1.0f, 1.0f, 1.0f});
    engine.render(frame);
    CHECK(frame[10].r == 1.0f);
    CHECK(frame[11].r == 1.0f);
}

TEST_CASE("engine: init resets time and seeds the rng")
{
    EngineConfig config;
    config.seed = 99;
    setup(config);
    uint32_t first = engine.rng().next_u32();
    engine.run_ticks(50);
    setup(config);
    CHECK(engine.time_us() == 0);
    CHECK(engine.rng().next_u32() == first);
}
