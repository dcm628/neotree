#include "neotree/geometry.hpp"

#include <algorithm>
#include <cmath>

namespace neotree {

LedPoint led_point_from_cylindrical(float z_mm, float radius_mm, float angle_deg, PositionSource source)
{
    float a = angle_deg * (pi / 180.0f);
    return {{radius_mm * std::cos(a), radius_mm * std::sin(a), z_mm}, source};
}

void LedGeometry::reset(uint16_t count)
{
    count_ = std::min(count, max_leds);
    positioned_ = 0;
    mapped_ = 0;
    bounds_ = {};
    for (uint16_t i = 0; i < max_leds; i++)
    {
        source_[i] = PositionSource::none;
        x_[i] = y_[i] = z_[i] = radius_[i] = angle_[i] = height01_[i] = 0.0f;
    }
}

void LedGeometry::set(uint16_t index, const LedPoint &point)
{
    if (index >= count_)
    {
        return;
    }
    source_[index] = point.source;
    x_[index] = point.pos.x;
    y_[index] = point.pos.y;
    z_[index] = point.pos.z;
}

void LedGeometry::finalize()
{
    positioned_ = 0;
    mapped_ = 0;
    for (uint16_t i = 0; i < count_; i++)
    {
        if (source_[i] == PositionSource::none)
        {
            continue;
        }
        if (source_[i] == PositionSource::mapped)
        {
            mapped_++;
        }
        radius_[i] = std::sqrt(x_[i] * x_[i] + y_[i] * y_[i]);
        float a = std::atan2(y_[i], x_[i]);
        angle_[i] = a < 0.0f ? a + two_pi : a;

        Vec3 p = position(i);
        if (positioned_ == 0)
        {
            bounds_ = {p, p};
        }
        else
        {
            bounds_.min = {std::min(bounds_.min.x, p.x), std::min(bounds_.min.y, p.y), std::min(bounds_.min.z, p.z)};
            bounds_.max = {std::max(bounds_.max.x, p.x), std::max(bounds_.max.y, p.y), std::max(bounds_.max.z, p.z)};
        }
        z_order_[positioned_++] = i;
    }

    float height = bounds_.max.z - bounds_.min.z;
    for (float &e : envelope_)
    {
        e = 0.0f;
    }
    for (uint16_t k = 0; k < positioned_; k++)
    {
        uint16_t i = z_order_[k];
        height01_[i] = height > 0.0f ? (z_[i] - bounds_.min.z) / height : 0.0f;
    }

    // Ties broken by index so the order is identical on every platform.
    std::sort(z_order_, z_order_ + positioned_, [this](uint16_t a, uint16_t b) {
        return z_[a] < z_[b] || (z_[a] == z_[b] && a < b);
    });
    for (uint16_t k = 0; k < positioned_; k++)
    {
        z_sorted_[k] = z_[z_order_[k]];
    }

    // Envelope: per height band, the radius 85% of its LEDs are inside (so a
    // few stray outliers don't define the surface), from a 10mm histogram -
    // small enough for the Pico's stack. Gaps are filled from the nearest
    // bands, then lightly smoothed.
    float raw[envelope_bins] = {};
    bool have[envelope_bins] = {};
    constexpr int radius_buckets = 128;   // 0..1280 mm
    constexpr float bucket_mm = 10.0f;
    uint16_t k0 = 0;
    for (int b = 0; b < envelope_bins; b++)
    {
        uint16_t hist[radius_buckets] = {};
        uint16_t n = 0;
        uint16_t k1 = k0;
        while (k1 < positioned_ &&
               std::min(envelope_bins - 1, static_cast<int>(height01_[z_order_[k1]] * envelope_bins)) == b)
        {
            int r = std::min(radius_buckets - 1, static_cast<int>(radius_[z_order_[k1]] / bucket_mm));
            hist[r]++;
            n++;
            k1++;
        }
        k0 = k1;
        if (n == 0)
        {
            continue;
        }
        uint16_t want = static_cast<uint16_t>(std::ceil(0.85f * n));
        uint16_t seen = 0;
        for (int r = 0; r < radius_buckets; r++)
        {
            seen += hist[r];
            if (seen >= want)
            {
                raw[b] = (static_cast<float>(r) + 1.0f) * bucket_mm;
                break;
            }
        }
        have[b] = true;
    }
    for (int b = 0; b < envelope_bins; b++)
    {
        if (have[b])
        {
            continue;
        }
        int lo = b - 1, hi = b + 1;
        while (lo >= 0 && !have[lo]) lo--;
        while (hi < envelope_bins && !have[hi]) hi++;
        float a = lo >= 0 ? raw[lo] : (hi < envelope_bins ? raw[hi] : 0.0f);
        float c = hi < envelope_bins ? raw[hi] : a;
        float t = (lo >= 0 && hi < envelope_bins) ? static_cast<float>(b - lo) / static_cast<float>(hi - lo) : 0.0f;
        raw[b] = a + (c - a) * t;
    }
    for (int b = 0; b < envelope_bins; b++)
    {
        float prev = raw[std::max(b - 1, 0)];
        float next = raw[std::min(b + 1, envelope_bins - 1)];
        envelope_[b] = 0.25f * prev + 0.5f * raw[b] + 0.25f * next;
    }

    build_bands();
}

void LedGeometry::build_bands()
{
    const float height = bounds_.max.z - bounds_.min.z;
    band_scale_ = height > 0.0f ? static_cast<float>(cull_bands) / height : 0.0f;
    uint16_t counts[cull_bands] = {};
    for (uint16_t k = 0; k < positioned_; k++)
    {
        counts[band_of(z_[z_order_[k]])]++;
    }
    band_start_[0] = 0;
    for (int b = 0; b < cull_bands; b++)
    {
        band_start_[b + 1] = static_cast<uint16_t>(band_start_[b] + counts[b]);
    }
    uint16_t fill[cull_bands];
    for (int b = 0; b < cull_bands; b++)
    {
        fill[b] = band_start_[b];
    }
    for (uint16_t k = 0; k < positioned_; k++)
    {
        const uint16_t i = z_order_[k];
        band_leds_[fill[band_of(z_[i])]++] = i;
    }
    // Ties broken by index so the order is identical on every platform.
    for (int b = 0; b < cull_bands; b++)
    {
        std::sort(band_leds_ + band_start_[b], band_leds_ + band_start_[b + 1], [this](uint16_t a, uint16_t c) {
            return angle_[a] < angle_[c] || (angle_[a] == angle_[c] && a < c);
        });
    }
    for (uint16_t k = 0; k < positioned_; k++)
    {
        band_angle_[k] = angle_[band_leds_[k]];
    }
}

float LedGeometry::envelope_radius(float z) const
{
    float height = bounds_.max.z - bounds_.min.z;
    if (positioned_ == 0 || height <= 0.0f)
    {
        return envelope_[0];
    }
    // Bin centres at (b + 0.5) / bins; linear between them, flat past the ends.
    float f = (z - bounds_.min.z) / height * envelope_bins - 0.5f;
    if (f <= 0.0f)
    {
        return envelope_[0];
    }
    if (f >= envelope_bins - 1)
    {
        return envelope_[envelope_bins - 1];
    }
    int b = static_cast<int>(f);
    float t = f - static_cast<float>(b);
    return envelope_[b] + (envelope_[b + 1] - envelope_[b]) * t;
}

std::span<const uint16_t> LedGeometry::in_z_range(float z_min, float z_max) const
{
    if (z_max < z_min)
    {
        return {};
    }
    const float *begin = std::lower_bound(z_sorted_, z_sorted_ + positioned_, z_min);
    const float *end = std::upper_bound(begin, z_sorted_ + positioned_, z_max);
    return {z_order_ + (begin - z_sorted_), static_cast<size_t>(end - begin)};
}

}  // namespace neotree
