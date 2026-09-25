#include "doctest/doctest.h"
#include "neotree/color.hpp"

using namespace neotree;

TEST_CASE("color: every byte survives byte -> unit -> byte")
{
    for (int b = 0; b < 256; b++)
    {
        CHECK(unit_to_byte(to_rgb(static_cast<uint8_t>(b), 0, 0).r) == b);
    }
    CHECK(unit_to_byte(-0.5f) == 0);
    CHECK(unit_to_byte(3.0f) == 255);
}

TEST_CASE("color: opaque normal blend of any byte over any byte gives exactly the top byte")
{
    // What the Canvas relies on: paint over background must send the paint's
    // byte unchanged, exactly as the pre-engine firmware did.
    for (int below = 0; below < 256; below++)
    {
        for (int top = 0; top < 256; top++)
        {
            Rgb dst = to_rgb(static_cast<uint8_t>(below), 0, 0);
            blend_into(dst, to_rgb(static_cast<uint8_t>(top), 0, 0), 1.0f, Blend::normal);
            REQUIRE(unit_to_byte(dst.r) == top);
        }
    }
}

TEST_CASE("color: blend modes")
{
    const Rgb grey{0.5f, 0.5f, 0.5f};
    const Rgb red{1.0f, 0.0f, 0.0f};

    Rgb d = grey;
    blend_into(d, red, 0.0f, Blend::normal);
    CHECK(d.r == 0.5f);   // zero coverage changes nothing

    d = grey;
    blend_into(d, red, 0.5f, Blend::normal);
    CHECK(d.r == doctest::Approx(0.75f));
    CHECK(d.g == doctest::Approx(0.25f));

    d = grey;
    blend_into(d, red, 1.0f, Blend::add);
    CHECK(d.r == doctest::Approx(1.5f));   // not clamped until the master stage
    CHECK(d.g == doctest::Approx(0.5f));

    d = grey;
    blend_into(d, red, 1.0f, Blend::max);
    CHECK(d.r == doctest::Approx(1.0f));
    CHECK(d.g == doctest::Approx(0.5f));

    d = grey;
    blend_into(d, red, 1.0f, Blend::multiply);
    CHECK(d.r == doctest::Approx(0.5f));
    CHECK(d.g == doctest::Approx(0.0f));

    d = grey;
    blend_into(d, red, 0.1f, Blend::replace);
    CHECK(d.r == 1.0f);
    CHECK(d.g == 0.0f);
    d = grey;
    blend_into(d, red, 0.0f, Blend::replace);
    CHECK(d.r == 0.5f);
}

TEST_CASE("color: hsv primaries and wrapping")
{
    Rgb r = hsv(0.0f, 1.0f, 1.0f);
    CHECK(r.r == doctest::Approx(1.0f));
    CHECK(r.g == doctest::Approx(0.0f));
    Rgb g = hsv(120.0f, 1.0f, 1.0f);
    CHECK(g.g == doctest::Approx(1.0f));
    CHECK(g.r == doctest::Approx(0.0f));
    Rgb b = hsv(-120.0f, 1.0f, 1.0f);   // same as 240
    CHECK(b.b == doctest::Approx(1.0f));
    Rgb white = hsv(77.0f, 0.0f, 0.8f);
    CHECK(white.r == doctest::Approx(0.8f));
    CHECK(white.g == doctest::Approx(0.8f));
    CHECK(white.b == doctest::Approx(0.8f));
}
