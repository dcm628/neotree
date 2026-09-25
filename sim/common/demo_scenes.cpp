#include "demo_scenes.hpp"

#include "neotree/demos.hpp"

namespace neotree::sim {

namespace {

// The engine's built-in demos go in slot 1, above where the Canvas sits on
// the tree (slot 0).
constexpr uint8_t demo_slot = 1;

// canvas: what the tree shows at boot - an opaque background pixel layer
// (here a green/blue/purple/red pattern by LED index, like init_my_tree's
// ranges) and an empty paint layer above it. Host-only: the firmware builds
// its own from the real boot pattern.
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

}  // namespace

std::string demo_scene_names() { return std::string("empty, canvas, ") + demo_names(); }

bool setup_demo(Engine &engine, const std::string &name)
{
    engine.scene().clear();
    engine.destroy_entities_in_slot(-1);
    engine.forces() = Forces{};
    if (name == "empty")
    {
        return true;
    }
    if (name == "canvas")
    {
        setup_canvas(engine);
        return true;
    }
    Demo demo;
    if (!demo_from_name(name.c_str(), demo))
    {
        return false;
    }
    setup_demo(engine, demo, demo_slot);
    return true;
}

void update_demo(Engine &engine, const std::string &name)
{
    Demo demo;
    if (demo_from_name(name.c_str(), demo))
    {
        update_demo(engine, demo, demo_slot);
    }
}

}  // namespace neotree::sim
