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
// Branch-free min / max / clamp for the per-LED loops. On the Pico (Cortex-
// M33) std::fmin / std::fmax are single VMINNM / VMAXNM instructions, where
// std::min / std::max / std::clamp compile to compare-and-branch - an FPU
// flag transfer and often a taken branch each, several times the cost. Same
// results for finite values.
inline float min_f(float a, float b) { return std::fmin(a, b); }
inline float max_f(float a, float b) { return std::fmax(a, b); }
inline float clamp_f(float v, float lo, float hi) { return std::fmin(std::fmax(v, lo), hi); }
inline float clamp01(float v) { return clamp_f(v, 0.0f, 1.0f); }

inline uint8_t unit_to_byte(float v)
{
    // Through uint32_t: the byte then goes straight to a register instead of
    // via the stack.
    return static_cast<uint8_t>(static_cast<uint32_t>(clamp01(v) * 255.0f + 0.5f));
}

// hue in degrees (any value, wraps), saturation and value 0..1.
inline Rgb hsv(float hue_deg, float s, float v)
{
    float h = hue_deg - 360.0f * std::floor(hue_deg / 360.0f);
    auto channel = [&](float n) {
        // fmod(k, 6) for k in [0, 12): at most one subtraction, exact for k
        // >= 6 (Sterbenz), so the same bits without fmodf (~85 cycles on the
        // Pico, three per LED for a rainbow).
        float k = n + h / 60.0f;
        if (k >= 6.0f)
        {
            k -= 6.0f;
        }
        return v - v * s * clamp01(min_f(k, 4.0f - k));
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
        dst = lerp(dst, Rgb{max_f(dst.r, src.r), max_f(dst.g, src.g), max_f(dst.b, src.b)}, a);
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
