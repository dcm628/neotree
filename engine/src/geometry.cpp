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
