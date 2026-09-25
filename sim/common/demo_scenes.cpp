#include "demo_scenes.hpp"

#include <cctype>
#include <cmath>

#include "neotree/direct.hpp"

namespace neotree::sim {

namespace {

// "Holiday show" -> "holiday_show"
std::string slug(const char *name)
{
    std::string s;
    for (const char *p = name; *p != '\0'; p++)
    {
        s += *p == ' ' ? '_' : static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));
    }
    return s;
}

}  // namespace

void seed_canvas(std::span<Rgba8> background)
{
    const Rgba8 pattern[] = {{0, 140, 0, 255}, {0, 0, 180, 255}, {160, 0, 120, 255}, {180, 0, 0, 255}};
    for (size_t i = 0; i < background.size(); i++)
    {
        background[i] = pattern[(i / 25) % 4];
    }
}

// What a phone does on the Play tab (docs/RENDERER.md 12), scripted: a
// brush spiral painted up the tree and six balls flicked in.
static void play_demo(Engine &engine)
{
    SceneSpec scene;
    scene.specs[0] = SlotSpec::of("solid").set("color", Rgb{0.0f, 0.0f, 0.03f});   // dark, so the strokes show
    scene.specs[3] = SlotSpec::of("play").set("fade", 30.0f);
    engine.director().apply_scene(engine, scene, Transition::cut);
    const LedGeometry &g = engine.geometry();
    const float z0 = g.bounds().min.z + 150.0f;
    const float z1 = g.bounds().max.z - 250.0f;
    constexpr uint8_t phone = 1;
    BrushSample s;
    s.radius_mm = 110.0f;
    s.pen_down = true;
    for (int k = 0; k <= 240; k++)
    {
        const float t = static_cast<float>(k) / 240.0f;
        const float z = z0 + (z1 - z0) * t;
        const float r = g.envelope_radius(z);
        const float a = 2.5f * two_pi * t;
        s.pos = {r * std::cos(a), r * std::sin(a), z};
        s.color = hsv(360.0f * t, 1.0f, 1.0f);
        direct_brush(engine, 3, phone, 0, s);
    }
    for (uint8_t b = 0; b < 6; b++)
    {
        const float a = two_pi * b / 6.0f;
        direct_spawn(engine, 3, phone, b, DirectKind::ball, {250.0f * std::cos(a), 250.0f * std::sin(a), z1},
                     {900.0f * std::cos(a), 900.0f * std::sin(a), 1500.0f}, hsv(60.0f * b, 0.9f, 1.0f), 0.0f);
    }
}

int scene_count() { return 1 + preset_count() + builtin_show_count() + mode_count(); }

std::string scene_name(int index)
{
    if (index <= 0)
    {
        return "empty";
    }
    if (index <= preset_count())
    {
        return slug(preset_at(static_cast<uint8_t>(index - 1)).name);
    }
    index -= preset_count();
    if (index <= builtin_show_count())
    {
        return slug(builtin_show_at(static_cast<uint8_t>(index - 1)).name);
    }
    const ModeDef *def = mode_at(static_cast<uint8_t>(index - 1 - builtin_show_count()));
    return def != nullptr ? def->id : "empty";
}

std::string scene_names()
{
    std::string s;
    for (int i = 0; i < scene_count(); i++)
    {
        s += (i ? ", " : "") + scene_name(i);
    }
    return s;
}

bool setup_scene(Engine &engine, const std::string &name)
{
    if (name == "empty")
    {
        engine.director().apply_scene(engine, SceneSpec{}, Transition::cut);
        return true;
    }
    if (name == "play_demo")
    {
        play_demo(engine);
        return true;
    }
    for (uint8_t i = 0; i < preset_count(); i++)
    {
        if (slug(preset_at(i).name) == name)
        {
            engine.director().apply_scene(engine, preset_at(i), Transition::cut);
            return true;
        }
    }
    const Library &lib = engine.director().library();
    for (uint8_t i = 0; i < lib.show_count(); i++)
    {
        if (slug(lib.show(i).name) == name)
        {
            return engine.director().play_show(engine, i);
        }
    }
    uint8_t mode = find_mode(name.c_str());
    if (mode == no_mode)
    {
        return false;
    }
    SceneSpec one;
    one.specs[0] = SlotSpec::of(name.c_str());
    engine.director().apply_scene(engine, one, Transition::cut);
    return true;
}

}  // namespace neotree::sim
