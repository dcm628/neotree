#pragma once
// Basic value types shared across the engine.

#include <cstdint>

namespace neotree {

// Linear-light color, nominally 0..1 per channel. Values above 1 are allowed
// while compositing (additive blends); the master stage clamps.
struct Rgb
{
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
};

// World-space position or direction, in millimeters. z is up along the trunk;
// x/y are horizontal with the trunk at x = y = 0.
struct Vec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

constexpr float pi = 3.14159265358979323846f;
constexpr float two_pi = 2.0f * pi;

}  // namespace neotree
