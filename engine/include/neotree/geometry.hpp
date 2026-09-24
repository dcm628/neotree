#pragma once
// Where every LED is, precomputed once in the forms the renderer needs:
// cartesian mm, cylindrical (radius, angle), normalized height, and an index
// sorted by height so a shape only has to test the LEDs in its z-extent.

#include <cstdint>
#include <span>

#include "neotree/types.hpp"

namespace neotree {

enum class PositionSource : uint8_t
{
    none,        // unknown - skipped by anything position-based
    mapped,      // measured by the mapping rig
    synthetic,   // generated stand-in (host simulator, until remapping)
};

struct LedPoint
{
    Vec3 pos;
    PositionSource source = PositionSource::none;
};

// Cylindrical input as the mapping pipeline stores it (z and radius in mm,
// angle in degrees counter-clockwise from +x).
LedPoint led_point_from_cylindrical(float z_mm, float radius_mm, float angle_deg,
                                    PositionSource source = PositionSource::mapped);

struct Bounds
{
    Vec3 min;
    Vec3 max;
};

class LedGeometry
{
public:
    static constexpr uint16_t max_leds = 1000;

    // Build in three steps so callers never need a whole LedPoint array on
    // the stack: reset(count), set(i, point) for each LED, then finalize().
    void reset(uint16_t count);
    void set(uint16_t index, const LedPoint &point);
    void finalize();

    uint16_t count() const { return count_; }
    uint16_t positioned_count() const { return positioned_; }
    uint16_t mapped_count() const { return mapped_; }

    PositionSource source(uint16_t i) const { return source_[i]; }
    bool has_position(uint16_t i) const { return source_[i] != PositionSource::none; }

    Vec3 position(uint16_t i) const { return {x_[i], y_[i], z_[i]}; }
    float x(uint16_t i) const { return x_[i]; }
    float y(uint16_t i) const { return y_[i]; }
    float z(uint16_t i) const { return z_[i]; }
    float radius(uint16_t i) const { return radius_[i]; }
    float angle(uint16_t i) const { return angle_[i]; }     // radians, [0, 2*pi)
    float height01(uint16_t i) const { return height01_[i]; }   // 0 = lowest positioned LED, 1 = highest

    // Over positioned LEDs only. All zero if none are positioned.
    const Bounds &bounds() const { return bounds_; }

    // Positioned LED indices, ascending by z.
    std::span<const uint16_t> z_order() const { return {z_order_, positioned_}; }

    // The positioned LEDs with z_min <= z <= z_max, as a slice of z_order().
    std::span<const uint16_t> in_z_range(float z_min, float z_max) const;

private:
    uint16_t count_ = 0;
    uint16_t positioned_ = 0;
    uint16_t mapped_ = 0;
    Bounds bounds_{};

    PositionSource source_[max_leds] = {};
    float x_[max_leds] = {};
    float y_[max_leds] = {};
    float z_[max_leds] = {};
    float radius_[max_leds] = {};
    float angle_[max_leds] = {};
    float height01_[max_leds] = {};

    uint16_t z_order_[max_leds] = {};
    float z_sorted_[max_leds] = {};   // z_[z_order_[k]], for the range search
};

}  // namespace neotree
