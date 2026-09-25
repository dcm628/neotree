#include "neotree/mask.hpp"

#include <algorithm>
#include <cmath>

#include "neotree/hot.hpp"

namespace neotree {

Mask Mask::make_box(Vec3 min, Vec3 max, float feather_mm)
{
    Mask m;
    m.shape = MaskShape::box;
    m.box_min = min;
    m.box_max = max;
    m.feather_mm = feather_mm;
    return m;
}

Mask Mask::make_cylinder(float z_min, float z_max, float radius_min, float radius_max, float angle_min,
                         float angle_max, float feather_mm)
{
    Mask m;
    m.shape = MaskShape::cylinder;
    m.z_min = z_min;
    m.z_max = z_max;
    m.radius_min = radius_min;
    m.radius_max = radius_max;
    m.angle_min = angle_min;
    m.angle_max = angle_max;
    m.feather_mm = feather_mm;
    return m;
}

namespace {

// How far v is outside [lo, hi] (0 when inside).
float outside(float v, float lo, float hi) { return std::max({lo - v, v - hi, 0.0f}); }

// Angular distance (radians) from a to the arc [lo, hi], which wraps through
// 0 when lo > hi. 0 when a is on the arc.
float angle_outside(float a, float lo, float hi)
{
    bool wraps = lo > hi;
    bool inside = wraps ? (a >= lo || a <= hi) : (a >= lo && a <= hi);
    if (inside)
    {
        return 0.0f;
    }
    auto gap = [](float from, float to) {   // shortest way round, 0..pi
        float d = std::fabs(from - to);
        return std::min(d, two_pi - d);
    };
    return std::min(gap(a, lo), gap(a, hi));
}

float coverage_from_distance(float d, float feather)
{
    if (d <= 0.0f)
    {
        return 1.0f;
    }
    if (feather <= 0.0f)
    {
        return 0.0f;
    }
    return std::max(0.0f, 1.0f - d / feather);
}

}  // namespace

NEOTREE_HOT float mask_coverage(const Mask &mask, const LedGeometry &geometry, uint16_t i)
{
    if (mask.shape == MaskShape::none)
    {
        return 1.0f;
    }
    if (!geometry.has_position(i))
    {
        return 0.0f;
    }
    float d = 0.0f;   // distance outside the shape, mm
    switch (mask.shape)
    {
    case MaskShape::box:
    {
        float dx = outside(geometry.x(i), mask.box_min.x, mask.box_max.x);
        float dy = outside(geometry.y(i), mask.box_min.y, mask.box_max.y);
        float dz = outside(geometry.z(i), mask.box_min.z, mask.box_max.z);
        d = std::sqrt(dx * dx + dy * dy + dz * dz);
        break;
    }
    case MaskShape::cylinder:
    {
        float r = geometry.radius(i);
        float dz = outside(geometry.z(i), mask.z_min, mask.z_max);
        float dr = outside(r, mask.radius_min, mask.radius_max);
        // Angular miss as arc length at the LED's radius.
        float da = angle_outside(geometry.angle(i), mask.angle_min, mask.angle_max) * r;
        d = std::sqrt(dz * dz + dr * dr + da * da);
        break;
    }
    case MaskShape::none:
        break;
    }
    float c = coverage_from_distance(d, mask.feather_mm);
    return mask.invert ? 1.0f - c : c;
}

}  // namespace neotree
