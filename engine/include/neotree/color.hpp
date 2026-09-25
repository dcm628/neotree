#pragma once
// Colors and blending. Working values are LED drive levels, 0..1 per channel
// (1 = full PWM duty), so a byte sent today comes out as the same byte; the
// master stage's gamma (off by default) can make them perceptual later.

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "neotree/types.hpp"

namespace neotree {

// 8-bit color with coverage, as stored by pixel layers and sent by commands.
struct Rgba8
{
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t a = 0;   // 0 = transparent (shows what's below), 255 = opaque
};

constexpr float byte_to_unit = 1.0f / 255.0f;

// Final 8-bit output color (Engine::render_bytes).
struct Rgb8
{
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
};

inline Rgb to_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return {r * byte_to_unit, g * byte_to_unit, b * byte_to_unit};
}

// 0..1 -> 0..255, rounded. Exact inverse of to_rgb for every byte value.
inline uint8_t unit_to_byte(float v)
{
    return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

// hue in degrees (any value, wraps), saturation and value 0..1.
inline Rgb hsv(float hue_deg, float s, float v)
{
    float h = hue_deg - 360.0f * std::floor(hue_deg / 360.0f);
    auto channel = [&](float n) {
        float k = std::fmod(n + h / 60.0f, 6.0f);
        return v - v * s * std::clamp(std::min(k, 4.0f - k), 0.0f, 1.0f);
    };
    return {channel(5.0f), channel(3.0f), channel(1.0f)};
}

inline Rgb lerp(Rgb a, Rgb b, float t)
{
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

enum class Blend : uint8_t
{
    normal,     // src over dst by coverage
    add,        // dst + src * coverage (lights add up; the master stage clamps)
    max,        // per channel, the brighter of dst and src (faded by coverage)
    multiply,   // dst * src (faded by coverage) - tints and darkens
    replace,    // src wherever coverage > 0, ignoring dst
};

// Blends src into dst with coverage a (0..1, already including opacity).
inline void blend_into(Rgb &dst, Rgb src, float a, Blend mode)
{
    switch (mode)
    {
    case Blend::normal:
        dst = lerp(dst, src, a);
        break;
    case Blend::add:
        dst = {dst.r + src.r * a, dst.g + src.g * a, dst.b + src.b * a};
        break;
    case Blend::max:
        dst = lerp(dst, Rgb{std::max(dst.r, src.r), std::max(dst.g, src.g), std::max(dst.b, src.b)}, a);
        break;
    case Blend::multiply:
        dst = lerp(dst, Rgb{dst.r * src.r, dst.g * src.g, dst.b * src.b}, a);
        break;
    case Blend::replace:
        if (a > 0.0f)
        {
            dst = src;
        }
        break;
    }
}

}  // namespace neotree
