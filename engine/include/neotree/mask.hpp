#pragma once
// Masks: a shape that limits where a layer (or slot) shows. Coverage is 1
// inside, 0 outside, and ramps over feather_mm at the edge so moving or
// resizing a mask doesn't pop LEDs on and off.

#include <cstdint>

#include "neotree/geometry.hpp"
#include "neotree/types.hpp"

namespace neotree {

enum class MaskShape : uint8_t
{
    none,       // everything (no mask)
    box,        // axis-aligned box in world mm
    cylinder,   // height band x radius band x angle slice around the trunk
};

struct Mask
{
    MaskShape shape = MaskShape::none;
    bool invert = false;
    float feather_mm = 0.0f;

    // box
    Vec3 box_min{};
    Vec3 box_max{};

    // cylinder. Angles in radians, [0, 2pi); angle_min > angle_max wraps
    // through 0 (e.g. 350 deg..10 deg). A full circle is 0..2pi.
    float z_min = 0.0f;
    float z_max = 0.0f;
    float radius_min = 0.0f;
    float radius_max = 1.0e9f;
    float angle_min = 0.0f;
    float angle_max = two_pi;

    static Mask make_box(Vec3 min, Vec3 max, float feather_mm = 0.0f);
    static Mask make_cylinder(float z_min, float z_max, float radius_min, float radius_max, float angle_min,
                              float angle_max, float feather_mm = 0.0f);
};

// Coverage of LED i, 0..1. LEDs without a position are outside every mask
// shape (inverted or not); with shape none every LED is covered.
float mask_coverage(const Mask &mask, const LedGeometry &geometry, uint16_t i);

}  // namespace neotree
