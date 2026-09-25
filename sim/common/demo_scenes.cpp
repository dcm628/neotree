#include "demo_scenes.hpp"

#include <cmath>

namespace neotree::sim {

namespace {

float deg(float d) { return d * pi / 180.0f; }

// canvas: what the tree shows at boot today - an opaque background pixel
// layer (here a green/red/blue pattern by LED index, like init_my_tree's
// ranges) and an empty paint layer above it.
void setup_canvas(Engine &engine)
{
    Scene &scene = engine.scene();
    scene.add_layer(0, LayerType::pixel);
    scene.add_layer(0, LayerType::pixel);
    auto bg = scene.pixels(0, 0);
    const Rgba8 pattern[] = {{0, 140, 0, 255}, {0, 0, 180, 255}, {160, 0, 120, 255}, {180, 0, 0, 255}};
    for (size_t i = 0; i < bg.size(); i++)
    {
        bg[i] = pattern[(i / 25) % 4];
    }
}

// layers: every layer type, mask and a few blend modes at once.
//   slot 0: night-sky height gradient
//   slot 1: spinning rainbow, masked to a feathered band that rides up and
//           down the tree (update_demo moves it)
//   slot 2: warm white "tips" - a solid layer masked to the outer shell,
//           blended with max so it only brightens
//   slot 3: sparkles - a pixel layer with a few random LEDs set each frame
void setup_layers(Engine &engine)
{
    Scene &scene = engine.scene();

    scene.add_layer(0, LayerType::field);
    Layer *sky = scene.layer(0, 0);
    sky->field.kind = FieldKind::height_gradient;
    sky->field.color_a = {0.02f, 0.02f, 0.12f};
    sky->field.color_b = {0.10f, 0.00f, 0.18f};

    scene.add_layer(1, LayerType::field);
    Layer *band = scene.layer(1, 0);
    band->field.kind = FieldKind::angle_rainbow;
    band->field.spin_rps = 0.3f;
    band->field.hue_cycles = 2.0f;
    band->mask = Mask::make_cylinder(0.0f, 300.0f, 0.0f, 10000.0f, 0.0f, two_pi, 150.0f);

    scene.add_layer(2, LayerType::solid);
    Layer *tips = scene.layer(2, 0);
    tips->color = {0.6f, 0.45f, 0.2f};
    tips->blend = Blend::max;
    tips->opacity = 0.5f;
    tips->mask = Mask::make_cylinder(-10000.0f, 10000.0f, 400.0f, 10000.0f, 0.0f, two_pi, 60.0f);

    scene.add_layer(3, LayerType::pixel);
    scene.layer(3, 0)->blend = Blend::add;
}

void update_layers(Engine &engine)
{
    Scene &scene = engine.scene();
    const Bounds &b = engine.geometry().bounds();
    float t = static_cast<float>(static_cast<double>(engine.time_us()) * 1e-6);

    // Band rides up and down once every 8 s.
    Layer *band = scene.layer(1, 0);
    float h = 0.5f - 0.5f * std::cos(two_pi * t / 8.0f);
    float z = b.min.z + h * (b.max.z - b.min.z);
    band->mask.z_min = z - 150.0f;
    band->mask.z_max = z + 150.0f;

    // Sparkles: fade everything, then light a few new ones.
    auto px = scene.pixels(3, 0);
    for (Rgba8 &p : px)
    {
        p.a = static_cast<uint8_t>(p.a * 0.85f);
    }
    Rng &rng = engine.rng();
    for (int k = 0; k < 4; k++)
    {
        px[rng.below(static_cast<uint32_t>(engine.geometry().count()))] = {255, 255, 255, 255};
    }
}

// wedge: a lighthouse beam - a solid layer masked to a slice around the
// trunk, turned by update_demo.
void setup_wedge(Engine &engine)
{
    Scene &scene = engine.scene();
    scene.add_layer(0, LayerType::solid);
    scene.layer(0, 0)->color = {0.0f, 0.0f, 0.05f};
    scene.add_layer(0, LayerType::solid);
    Layer *beam = scene.layer(0, 1);
    beam->color = {1.0f, 0.8f, 0.4f};
    beam->mask = Mask::make_cylinder(-10000.0f, 10000.0f, 0.0f, 10000.0f, 0.0f, deg(40.0f), 80.0f);
}

void update_wedge(Engine &engine)
{
    Layer *beam = engine.scene().layer(0, 1);
    float t = static_cast<float>(static_cast<double>(engine.time_us()) * 1e-6);
    float a = std::fmod(t * 0.5f * two_pi, two_pi);   // half a turn per second
    beam->mask.angle_min = a;
    beam->mask.angle_max = std::fmod(a + deg(40.0f), two_pi);
}

}  // namespace

const char *demo_scene_names() { return "empty, canvas, layers, wedge"; }

bool setup_demo(Engine &engine, const std::string &name)
{
    engine.scene().clear();
    if (name == "empty")
    {
        return true;
    }
    if (name == "canvas")
    {
        setup_canvas(engine);
        return true;
    }
    if (name == "layers")
    {
        setup_layers(engine);
        return true;
    }
    if (name == "wedge")
    {
        setup_wedge(engine);
        return true;
    }
    return false;
}

void update_demo(Engine &engine, const std::string &name)
{
    if (name == "layers")
    {
        update_layers(engine);
    }
    else if (name == "wedge")
    {
        update_wedge(engine);
    }
}

}  // namespace neotree::sim
