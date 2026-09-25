#include <vector>

#include "doctest/doctest.h"
#include "neotree/demos.hpp"
#include "neotree/engine.hpp"

using namespace neotree;

namespace {

LedGeometry geometry;
Engine engine;

void setup()
{
    geometry.reset(30);
    for (uint16_t i = 0; i < 30; i++)
    {
        geometry.set(i, led_point_from_cylindrical(70.0f * i, 250.0f + 5.0f * i, 37.0f * i));
    }
    geometry.finalize();
    EngineConfig config;
    config.max_ticks_per_advance = 0;
    engine.init(geometry, config);
}

}  // namespace

TEST_CASE("render_bytes matches render + rounding, for every demo and master setting")
{
    for (int d = 0; d < static_cast<int>(Demo::count); d++)
    {
        for (float brightness : {1.0f, 0.4f})
        {
            for (float gamma : {1.0f, 2.2f})
            {
                setup();
                setup_demo(engine, static_cast<Demo>(d), 1);
                engine.master().brightness = brightness;
                engine.master().gamma = gamma;
                engine.advance(1'500'000);
                update_demo(engine, static_cast<Demo>(d), 1);
                std::vector<Rgb> f(geometry.count());
                std::vector<Rgb8> b(geometry.count());
                engine.render(f);
                engine.render_bytes(b);
                for (uint16_t i = 0; i < geometry.count(); i++)
                {
                    CHECK(b[i].r == unit_to_byte(f[i].r));
                    CHECK(b[i].g == unit_to_byte(f[i].g));
                    CHECK(b[i].b == unit_to_byte(f[i].b));
                }
            }
        }
    }
}

TEST_CASE("render_bytes: lights off is all zero")
{
    setup();
    setup_demo(engine, Demo::layers, 1);
    engine.master().output_enabled = false;
    std::vector<Rgb8> b(geometry.count(), Rgb8{9, 9, 9});
    engine.render_bytes(b);
    for (const Rgb8 &px : b)
    {
        CHECK(px.r == 0);
        CHECK(px.g == 0);
        CHECK(px.b == 0);
    }
}
