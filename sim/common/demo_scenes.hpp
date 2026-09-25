#pragma once
// Scenes for the host tools, by name: a preset or a built-in show
// (neotree/library.hpp - by its name in lowercase with underscores, e.g.
// "holiday_show", "holiday_evening"), a single mode id (e.g. "snow", run in
// slot 0), or "empty".

#include <string>

#include "neotree/engine.hpp"

namespace neotree::sim {

// Names accepted by setup_scene(), for help text.
std::string scene_names();

// Every accepted name: presets, then shows, then modes (for cycling in the viewer).
int scene_count();
std::string scene_name(int index);

// For EngineConfig::canvas_seed: the host has no boot pattern, so the
// Canvas background gets a green/blue/purple/red pattern by LED index, like
// init_my_tree's ranges.
void seed_canvas(std::span<Rgba8> background);

// Starts the named scene (cut, no fades). False if unknown.
bool setup_scene(Engine &engine, const std::string &name);

}  // namespace neotree::sim
