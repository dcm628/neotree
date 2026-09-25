#pragma once
// Where every LED is, precomputed once in the forms the renderer needs:
// cartesian mm, cylindrical (radius, angle), normalized height, and an index
// sorted by height so a shape only has to test the LEDs in its z-extent.

#include <algorithm>
#include <cmath>
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

    // The tree's outer envelope: the radius 85% of the LEDs at height z are
    // inside (from envelope_bins height bands, smoothed and interpolated).
    // Used for the outer boundary and the surface constraint.
    static constexpr int envelope_bins = 16;
    float envelope_radius(float z) const;

    // Culling for round shapes (spheres, shells): the positioned LEDs in
    // cull_bands height bands, each band sorted by angle. near(c, reach, fn)
    // calls fn(std::span<const uint16_t>) with the runs of LEDs that can be
    // within reach of c: only the bands it spans, and in each only the angles
    // it covers. A superset - the caller still tests distance - found with
    // two binary searches per band, instead of every LED in its height range.
    static constexpr int cull_bands = 20;
    template <typename Fn>
    void near(Vec3 c, float reach, Fn &&fn) const;

private:
    int band_of(float z) const;
    void build_bands();

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
    float envelope_[envelope_bins] = {};

    float band_scale_ = 0.0f;                    // bands per mm of height
    uint16_t band_start_[cull_bands + 1] = {};   // band b is band_leds_[band_start_[b] .. band_start_[b + 1])
    uint16_t band_leds_[max_leds] = {};          // positioned LEDs by band, then angle
    float band_angle_[max_leds] = {};            // angle_[band_leds_[k]], for the angle search
};

inline int LedGeometry::band_of(float z) const
{
    const float f = (z - bounds_.min.z) * band_scale_;
    if (!(f > 0.0f))
    {
        return 0;
    }
    return f >= static_cast<float>(cull_bands) ? cull_bands - 1 : static_cast<int>(f);
}

template <typename Fn>
void LedGeometry::near(Vec3 c, float reach, Fn &&fn) const
{
    if (positioned_ == 0)
    {
        return;
    }
    const int b0 = band_of(c.z - reach);
    const int b1 = band_of(c.z + reach);
    // Seen from the tree's axis, a ball of radius reach at distance rc spans
    // +-asin(reach / rc) around its own angle - every angle if it reaches the
    // axis. asin(x) <= x / sqrt(1 - x^2) (= tan(asin x)) is a cheaper upper
    // bound; with a small margin it stays a superset despite rounding. Past
    // a quarter turn, just take every angle.
    const float rc = std::sqrt(c.x * c.x + c.y * c.y);
    const float x = reach / std::fmax(rc, 1e-3f);
    bool all = x >= 0.7f;
    float lo = 0.0f;
    float hi = 0.0f;
    if (!all)
    {
        const float half = x / std::sqrt(1.0f - x * x) * 1.001f + 1e-3f;
        float theta = std::atan2(c.y, c.x);
        theta = theta < 0.0f ? theta + two_pi : theta;
        lo = theta - half;
        hi = theta + half;
    }
    for (int b = b0; b <= b1; b++)
    {
        const uint16_t s = band_start_[b];
        const uint16_t e = band_start_[b + 1];
        if (s == e)
        {
            continue;
        }
        if (all)
        {
            fn(std::span<const uint16_t>(band_leds_ + s, static_cast<size_t>(e - s)));
            continue;
        }
        // The LEDs of this band with a0 <= angle <= a1.
        auto run = [&](float a0, float a1) {
            const float *first = std::lower_bound(band_angle_ + s, band_angle_ + e, a0);
            const float *last = std::upper_bound(first, band_angle_ + e, a1);
            if (first != last)
            {
                fn(std::span<const uint16_t>(band_leds_ + (first - band_angle_), static_cast<size_t>(last - first)));
            }
        };
        // Split where the window wraps past 0 / 2pi (half < pi: no overlap).
        if (lo < 0.0f)
        {
            run(lo + two_pi, two_pi);
            run(0.0f, hi);
        }
        else if (hi > two_pi)
        {
            run(lo, two_pi);
            run(0.0f, hi - two_pi);
        }
        else
        {
            run(lo, hi);
        }
    }
}

}  // namespace neotree
