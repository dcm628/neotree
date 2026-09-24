#include "doctest/doctest.h"
#include "neotree/random.hpp"

using neotree::Rng;

TEST_CASE("rng: same seed, same sequence; different seed, different sequence")
{
    Rng a(42), b(42), c(43);
    bool differs = false;
    for (int i = 0; i < 1000; i++)
    {
        uint32_t va = a.next_u32();
        CHECK(va == b.next_u32());
        differs |= va != c.next_u32();
    }
    CHECK(differs);
}

TEST_CASE("rng: reseed restarts the sequence")
{
    Rng a(7);
    uint32_t first = a.next_u32();
    a.next_u32();
    a.reseed(7);
    CHECK(a.next_u32() == first);
}

TEST_CASE("rng: sequence is pinned (catches accidental algorithm changes)")
{
    // Recorded from this implementation. Scenes saved with a seed replay the
    // same way only while this holds - change it deliberately, not by accident.
    // The firmware build must produce the same values (it uses only 32-bit
    // integer ops, so it will).
    Rng r(1);
    CHECK(r.next_u32() == 2442144158u);
    CHECK(r.next_u32() == 3238099751u);
    CHECK(r.next_u32() == 3819917871u);
}

TEST_CASE("rng: uniform, range and below stay in bounds and cover them")
{
    Rng r(3);
    float lo = 1.0f, hi = 0.0f;
    int buckets[10] = {};
    for (int i = 0; i < 100000; i++)
    {
        float u = r.uniform();
        REQUIRE(u >= 0.0f);
        REQUIRE(u < 1.0f);
        lo = u < lo ? u : lo;
        hi = u > hi ? u : hi;
        buckets[static_cast<int>(u * 10.0f)]++;

        float x = r.range(-5.0f, 5.0f);
        REQUIRE(x >= -5.0f);
        REQUIRE(x < 5.0f);

        REQUIRE(r.below(7) < 7u);
    }
    CHECK(lo < 0.001f);
    CHECK(hi > 0.999f);
    for (int n : buckets)
    {
        CHECK(n > 9000);   // expected 10000 each
        CHECK(n < 11000);
    }
}
