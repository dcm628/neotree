#pragma once
// Seeded pseudo-random numbers. xoshiro128** - 32-bit operations only, which
// suits the Cortex-M33, and the same seed gives the same sequence on the
// Pico and the PC.

#include <cstdint>

namespace neotree {

class Rng
{
public:
    explicit Rng(uint32_t seed = 1) { reseed(seed); }

    void reseed(uint32_t seed)
    {
        // splitmix32 expands one seed into four well-mixed state words (the
        // state must never be all zero).
        uint32_t x = seed;
        for (uint32_t &word : s_)
        {
            x += 0x9E3779B9u;
            uint32_t z = x;
            z = (z ^ (z >> 16)) * 0x85EBCA6Bu;
            z = (z ^ (z >> 13)) * 0xC2B2AE35u;
            word = z ^ (z >> 16);
        }
    }

    uint32_t next_u32()
    {
        const uint32_t result = rotl(s_[1] * 5u, 7) * 9u;
        const uint32_t t = s_[1] << 9;
        s_[2] ^= s_[0];
        s_[3] ^= s_[1];
        s_[1] ^= s_[2];
        s_[0] ^= s_[3];
        s_[2] ^= t;
        s_[3] = rotl(s_[3], 11);
        return result;
    }

    // Uniform in [0, 1): the top 24 bits, which a float represents exactly.
    float uniform() { return static_cast<float>(next_u32() >> 8) * (1.0f / 16777216.0f); }

    // Uniform in [lo, hi).
    float range(float lo, float hi) { return lo + (hi - lo) * uniform(); }

    // Uniform integer in [0, n). n must be > 0.
    uint32_t below(uint32_t n) { return static_cast<uint32_t>((static_cast<uint64_t>(next_u32()) * n) >> 32); }

private:
    static uint32_t rotl(uint32_t x, int k) { return (x << k) | (x >> (32 - k)); }
    uint32_t s_[4] = {};
};

}  // namespace neotree
