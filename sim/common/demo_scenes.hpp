#pragma once
// Demo scenes for the host tools, built from the engine's scene API. Until
// modes arrive (M5) these are plain setup functions; until entities arrive
// (M3) any motion comes from update() moving layer settings over time.

#include <string>

#include "neotree/engine.hpp"

namespace neotree::sim {

// Names accepted by setup_demo(), for help text.
const char *demo_scene_names();

// Replaces the engine's scene with the named demo. False if unknown.
bool setup_demo(Engine &engine, const std::string &name);

// Per-frame changes the demo makes from outside the engine (called with the
// engine's current simulation time).
void update_demo(Engine &engine, const std::string &name);

}  // namespace neotree::sim
