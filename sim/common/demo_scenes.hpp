#pragma once
// Demo scenes for the host tools: the engine's built-in demos
// (neotree/demos.hpp, shared with the firmware's DEMO command) plus the
// host-only "canvas" and "empty".

#include <string>

#include "neotree/engine.hpp"

namespace neotree::sim {

// Names accepted by setup_demo(), for help text.
std::string demo_scene_names();

// Replaces the engine's scene with the named demo. False if unknown.
bool setup_demo(Engine &engine, const std::string &name);

// Per-frame changes the demo makes from outside the engine (called with the
// engine's current simulation time).
void update_demo(Engine &engine, const std::string &name);

}  // namespace neotree::sim
