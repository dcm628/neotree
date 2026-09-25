#include <vector>

#include "doctest/doctest.h"
#include "neotree/engine.hpp"

using namespace neotree;

namespace {

LedGeometry geometry;
Engine engine;

// Four LEDs: 0 at z=0 (angle 0), 1 at z=1000 (angle 90), 2 at z=2000
// (angle 180), and 3 with no position.
void setup()
{
    geometry.reset(4);
    geometry.set(0, led_point_from_cylindrical(0.0f, 300.0f, 0.0f));
    geometry.set(1, led_point_from_cylindrical(1000.0f, 300.0f, 90.0f));
    geometry.set(2, led_point_from_cylindrical(2000.0f, 300.0f, 180.0f));
    geometry.finalize();
    EngineConfig config;
    config.max_ticks_per_advance = 0;
    engine.init(geometry, config);
}

std::vector<Rgb> render()
{
    std::vector<Rgb> out(4);
    engine.render(out);
    return out;
}

uint8_t byte_r(const Rgb &c) { return unit_to_byte(c.r); }

}  // namespace

TEST_CASE("scene: empty scene is black")
{
    setup();
    for (const Rgb &px : render())
    {
        CHECK(px.r == 0.0f);
        CHECK(px.g == 0.0f);
        CHECK(px.b == 0.0f);
    }
}

TEST_CASE("scene: solid layer covers every LED, positioned or not")
{
    setup();
    Scene &scene = engine.scene();
    int l = scene.add_layer(0, LayerType::solid);
    REQUIRE(l == 0);
    scene.layer(0, 0)->color = {0.2f, 0.4f, 0.6f};
    auto out = render();
    for (const Rgb &px : out)
    {
        CHECK(px.r == doctest::Approx(0.2f));
        CHECK(px.b == doctest::Approx(0.6f));
    }
}

TEST_CASE("scene: pixel layer - transparent until set, exact bytes, coverage blends")
{
    setup();
    Scene &scene = engine.scene();
    scene.add_layer(0, LayerType::solid);
    scene.layer(0, 0)->color = to_rgb(10, 20, 30);
    REQUIRE(scene.add_layer(0, LayerType::pixel) == 1);
    auto px = scene.pixels(0, 1);
    REQUIRE(px.size() == LedGeometry::max_leds);

    auto out = render();
    CHECK(byte_r(out[1]) == 10);   // transparent: background shows

    px[1] = {200, 0, 0, 255};
    px[2] = {200, 0, 0, 0};     // color set but transparent
    px[3] = {200, 0, 0, 255};   // unpositioned LEDs still take pixel colors
    out = render();
    CHECK(byte_r(out[0]) == 10);
    CHECK(byte_r(out[1]) == 200);
    CHECK(byte_r(out[2]) == 10);
    CHECK(byte_r(out[3]) == 200);

    px[1].a = 128;   // half coverage: halfway between 10 and 200
    out = render();
    CHECK(unit_to_byte(out[1].r) == doctest::Approx(105).epsilon(0.02));
}

TEST_CASE("scene: pixel buffers run out, and clearing a slot frees them")
{
    setup();
    Scene &scene = engine.scene();
    int made = 0;
    for (uint8_t s = 0; s < max_slots; s++)
    {
        for (uint8_t l = 0; l < max_layers_per_slot; l++)
        {
            made += scene.add_layer(s, LayerType::pixel) >= 0 ? 1 : 0;
        }
    }
    CHECK(made == max_pixel_buffers);
    CHECK(scene.pixel_buffers_free() == 0);
    scene.clear_slot(0);
    CHECK(scene.pixel_buffers_free() > 0);
    CHECK(scene.add_layer(0, LayerType::pixel) == 0);
}

TEST_CASE("scene: a slot holds at most max_layers_per_slot layers")
{
    setup();
    Scene &scene = engine.scene();
    for (uint8_t l = 0; l < max_layers_per_slot; l++)
    {
        CHECK(scene.add_layer(1, LayerType::solid) == l);
    }
    CHECK(scene.add_layer(1, LayerType::solid) == -1);
    CHECK(scene.add_layer(max_slots, LayerType::solid) == -1);
    CHECK(scene.layer(1, max_layers_per_slot) == nullptr);
}

TEST_CASE("scene: slots stack bottom to top; opacity and disabling")
{
    setup();
    Scene &scene = engine.scene();
    scene.add_layer(2, LayerType::solid);
    scene.layer(2, 0)->color = {1.0f, 0.0f, 0.0f};
    scene.add_layer(0, LayerType::solid);
    scene.layer(0, 0)->color = {0.0f, 1.0f, 0.0f};
    auto out = render();
    CHECK(out[0].r == doctest::Approx(1.0f));   // slot 2 is above slot 0
    CHECK(out[0].g == doctest::Approx(0.0f));

    scene.slot(2)->opacity = 0.5f;
    out = render();
    CHECK(out[0].r == doctest::Approx(0.5f));
    CHECK(out[0].g == doctest::Approx(0.5f));

    scene.slot(2)->enabled = false;
    out = render();
    CHECK(out[0].r == doctest::Approx(0.0f));
    CHECK(out[0].g == doctest::Approx(1.0f));
}

TEST_CASE("scene: masks limit a layer to a region")
{
    setup();
    Scene &scene = engine.scene();
    scene.add_layer(0, LayerType::solid);
    Layer *layer = scene.layer(0, 0);
    layer->color = {1.0f, 1.0f, 1.0f};
    layer->mask = Mask::make_cylinder(500.0f, 1500.0f, 0.0f, 1000.0f, 0.0f, two_pi);
    auto out = render();
    CHECK(out[0].r == 0.0f);
    CHECK(out[1].r == doctest::Approx(1.0f));
    CHECK(out[2].r == 0.0f);
    CHECK(out[3].r == 0.0f);   // no position: outside every mask

    layer->mask.invert = true;
    out = render();
    CHECK(out[0].r == doctest::Approx(1.0f));
    CHECK(out[1].r == 0.0f);
    CHECK(out[3].r == 0.0f);   // still outside - unpositioned LEDs never match a shape
}

TEST_CASE("scene: height gradient field runs bottom to top; skips unpositioned LEDs")
{
    setup();
    Scene &scene = engine.scene();
    scene.add_layer(0, LayerType::field);
    Layer *layer = scene.layer(0, 0);
    layer->field.kind = FieldKind::height_gradient;
    layer->field.color_a = {1.0f, 0.0f, 0.0f};
    layer->field.color_b = {0.0f, 0.0f, 1.0f};
    auto out = render();
    CHECK(out[0].r == doctest::Approx(1.0f));
    CHECK(out[1].r == doctest::Approx(0.5f));
    CHECK(out[1].b == doctest::Approx(0.5f));
    CHECK(out[2].b == doctest::Approx(1.0f));
    CHECK(out[3].r == 0.0f);
    CHECK(out[3].b == 0.0f);
}

TEST_CASE("scene: rainbow field spins with simulation time")
{
    setup();
    Scene &scene = engine.scene();
    scene.add_layer(0, LayerType::field);
    Layer *layer = scene.layer(0, 0);
    layer->field.kind = FieldKind::angle_rainbow;
    layer->field.spin_rps = 0.25f;   // a quarter turn per second
    auto before = render();
    CHECK(before[0].r == doctest::Approx(1.0f));   // angle 0 = hue 0 = red
    engine.advance(1'000'000);
    auto after = render();
    // After a quarter turn counter-clockwise, LED 1 (at 90 degrees) shows
    // what LED 0 showed.
    CHECK(after[1].r == doctest::Approx(before[0].r).epsilon(0.001));
    CHECK(after[1].g == doctest::Approx(before[0].g).epsilon(0.001));
    CHECK(after[1].b == doctest::Approx(before[0].b).epsilon(0.001));
}

TEST_CASE("scene: master - lights off, brightness, gamma, clamping")
{
    setup();
    Scene &scene = engine.scene();
    scene.add_layer(0, LayerType::solid);
    scene.layer(0, 0)->color = {0.5f, 0.5f, 0.5f};
    scene.add_layer(0, LayerType::solid);
    scene.layer(0, 1)->color = {0.8f, 0.0f, 0.0f};
    scene.layer(0, 1)->blend = Blend::add;

    auto out = render();
    CHECK(out[0].r == 1.0f);   // 1.3 clamped
    CHECK(out[0].g == doctest::Approx(0.5f));

    engine.master().brightness = 0.5f;
    out = render();
    CHECK(out[0].r == doctest::Approx(0.65f));   // brightness before the clamp
    CHECK(out[0].g == doctest::Approx(0.25f));

    engine.master().brightness = 1.0f;
    engine.master().gamma = 2.0f;
    out = render();
    CHECK(out[0].g == doctest::Approx(0.25f));

    engine.master().output_enabled = false;
    out = render();
    CHECK(out[0].r == 0.0f);
    CHECK(out[0].g == 0.0f);
    engine.master().output_enabled = true;
    engine.master().gamma = 1.0f;
    out = render();
    CHECK(out[0].g == doctest::Approx(0.5f));   // the scene was untouched
}
