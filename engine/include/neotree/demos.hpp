#pragma once
// Built-in demo scenes, shared by the firmware (DEMO protocol command) and
// the host tools. Until modes arrive (M5) these are plain setup functions
// that fill one slot; a few also change settings each frame (update_demo).

#include <cstdint>

#include "neotree/engine.hpp"

namespace neotree {

enum class Demo : uint8_t
{
    none = 0,           // clears the slot (and its entities)
    layers = 1,         // every layer type: gradient sky, rainbow band, warm tips, sparkles
    wedge = 2,          // lighthouse beam: a spinning wedge entity
    sweep_linear = 3,   // today's Pi sweeps, as slab entities: constant speed, top to bottom
    sweep_gravity = 4,  //   falling from rest under gravity
    sweep_launch = 5,   //   launched from the bottom to just reach the top, then falling back
    bounce = 6,         // balls bouncing off the floor and the tree's outer envelope
    snow = 7,           // an emitter of drifting flakes that settle and fade; wind gusts
    orbit = 8,          // comets swirling around the trunk on the outer surface
    fireworks = 9,      // rockets from an emitter; a rule bursts each into sparks when it expires
    chain = 10,         // balls bounce off each other and every collision spawns another - held by the quota
    mixer = 11,         // red and blue balls bounce off their own color, pass through and swap with the other
    count
};

const char *demo_name(Demo demo);
// Looks up a demo by name; false if there's no such demo.
bool demo_from_name(const char *name, Demo &out);
// "layers, wedge, ..." for help text.
const char *demo_names();

// Replaces the contents of slot (and removes its entities) with the demo.
void setup_demo(Engine &engine, Demo demo, uint8_t slot);

// Per-frame changes made from outside the engine (only some demos need it).
void update_demo(Engine &engine, Demo demo, uint8_t slot);

}  // namespace neotree
