#include "demo_scenes.hpp"

#include <cctype>

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

int scene_count() { return 1 + preset_count() + mode_count(); }

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
    const ModeDef *def = mode_at(static_cast<uint8_t>(index - 1 - preset_count()));
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
    for (uint8_t i = 0; i < preset_count(); i++)
    {
        if (slug(preset_at(i).name) == name)
        {
            engine.director().set_base_scene(preset_at(0));   // "Colors", for reverts
            engine.director().apply_scene(engine, preset_at(i), Transition::cut);
            return true;
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
