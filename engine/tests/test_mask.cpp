#include "doctest/doctest.h"
#include "neotree/mask.hpp"

using namespace neotree;

namespace {

LedGeometry geometry;

void build(std::initializer_list<LedPoint> points)
{
    geometry.reset(static_cast<uint16_t>(points.size()));
    uint16_t i = 0;
    for (const LedPoint &p : points)
    {
        geometry.set(i++, p);
    }
    geometry.finalize();
}

float deg(float d) { return d * pi / 180.0f; }

}  // namespace

TEST_CASE("mask: none covers everything, including unpositioned LEDs")
{
    build({LedPoint{}});
    CHECK(mask_coverage(Mask{}, geometry, 0) == 1.0f);
}

TEST_CASE("mask: box, inclusive edges, feathered falloff")
{
    build({
        LedPoint{{0.0f, 0.0f, 0.0f}, PositionSource::mapped},
        LedPoint{{100.0f, 0.0f, 0.0f}, PositionSource::mapped},
        LedPoint{{150.0f, 0.0f, 0.0f}, PositionSource::mapped},
        LedPoint{{300.0f, 0.0f, 0.0f}, PositionSource::mapped},
    });
    Mask m = Mask::make_box({-100.0f, -100.0f, -100.0f}, {100.0f, 100.0f, 100.0f});
    CHECK(mask_coverage(m, geometry, 0) == 1.0f);
    CHECK(mask_coverage(m, geometry, 1) == 1.0f);   // on the edge
    CHECK(mask_coverage(m, geometry, 2) == 0.0f);

    m.feather_mm = 100.0f;
    CHECK(mask_coverage(m, geometry, 2) == doctest::Approx(0.5f));   // 50mm out of a 100mm feather
    CHECK(mask_coverage(m, geometry, 3) == 0.0f);

    m.invert = true;
    CHECK(mask_coverage(m, geometry, 0) == 0.0f);
    CHECK(mask_coverage(m, geometry, 2) == doctest::Approx(0.5f));
    CHECK(mask_coverage(m, geometry, 3) == 1.0f);
}

TEST_CASE("mask: cylinder height, radius and angle bands")
{
    build({
        led_point_from_cylindrical(500.0f, 300.0f, 45.0f),    // inside
        led_point_from_cylindrical(1500.0f, 300.0f, 45.0f),   // too high
        led_point_from_cylindrical(500.0f, 600.0f, 45.0f),    // too far out
        led_point_from_cylindrical(500.0f, 300.0f, 135.0f),   // wrong angle
    });
    Mask m = Mask::make_cylinder(0.0f, 1000.0f, 100.0f, 500.0f, deg(0.0f), deg(90.0f));
    CHECK(mask_coverage(m, geometry, 0) == 1.0f);
    CHECK(mask_coverage(m, geometry, 1) == 0.0f);
    CHECK(mask_coverage(m, geometry, 2) == 0.0f);
    CHECK(mask_coverage(m, geometry, 3) == 0.0f);
}

TEST_CASE("mask: angle slices can wrap through 0 degrees")
{
    build({
        led_point_from_cylindrical(0.0f, 300.0f, 355.0f),
        led_point_from_cylindrical(0.0f, 300.0f, 5.0f),
        led_point_from_cylindrical(0.0f, 300.0f, 180.0f),
    });
    Mask m = Mask::make_cylinder(-10.0f, 10.0f, 0.0f, 1000.0f, deg(350.0f), deg(10.0f));
    CHECK(mask_coverage(m, geometry, 0) == 1.0f);
    CHECK(mask_coverage(m, geometry, 1) == 1.0f);
    CHECK(mask_coverage(m, geometry, 2) == 0.0f);
}

TEST_CASE("mask: angular feather is measured as arc length at the LED's radius")
{
    // 10 degrees past the slice edge at r = 300mm is ~52mm of arc.
    build({led_point_from_cylindrical(0.0f, 300.0f, 100.0f)});
    Mask m = Mask::make_cylinder(-10.0f, 10.0f, 0.0f, 1000.0f, deg(0.0f), deg(90.0f), 104.7f);
    CHECK(mask_coverage(m, geometry, 0) == doctest::Approx(0.5f).epsilon(0.01));
}
