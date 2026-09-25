#include <cmath>
#include <vector>

#include "doctest/doctest.h"
#include "neotree/geometry.hpp"

using namespace neotree;

namespace {

LedGeometry geometry;   // static: it's ~30 KB

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

}  // namespace

TEST_CASE("geometry: near() yields every LED within reach, each exactly once, and few others")
{
    // A cone of LEDs like the tree: 900 on a spiral, radius shrinking with height.
    geometry.reset(900);
    for (uint16_t i = 0; i < 900; i++)
    {
        const float z = 2.2f * i;
        geometry.set(i, led_point_from_cylindrical(z, 600.0f - 0.25f * z + 30.0f * std::sin(0.37f * i), 137.5f * i));
    }
    geometry.finalize();

    uint32_t seed = 12345;
    auto rnd = [&]() {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>(seed >> 8) / 16777216.0f;
    };
    std::vector<int> seen(900);
    uint32_t yielded = 0;
    uint32_t inside = 0;
    for (int trial = 0; trial < 3000; trial++)
    {
        // Points around, inside and above the tree; reaches from tiny to the whole tree.
        const float r = 700.0f * rnd();
        const float a = two_pi * rnd();
        const Vec3 c{r * std::cos(a), r * std::sin(a), -200.0f + 2400.0f * rnd()};
        const float reach = trial % 10 == 0 ? 1500.0f * rnd() : 20.0f + 250.0f * rnd();
        std::fill(seen.begin(), seen.end(), 0);
        geometry.near(c, reach, [&](std::span<const uint16_t> leds) {
            for (uint16_t i : leds)
            {
                seen[i]++;
                yielded++;
            }
        });
        for (uint16_t i = 0; i < 900; i++)
        {
            const Vec3 p = geometry.position(i);
            const float d2 = (p.x - c.x) * (p.x - c.x) + (p.y - c.y) * (p.y - c.y) + (p.z - c.z) * (p.z - c.z);
            REQUIRE(seen[i] <= 1);
            if (d2 <= reach * reach)
            {
                inside++;
                REQUIRE(seen[i] == 1);
            }
        }
    }
    MESSAGE("near(): yielded " << yielded << " LEDs for " << inside << " within reach");
    CHECK(yielded < 4 * inside);   // culls: far fewer than the ~height-band superset
}

TEST_CASE("geometry: cylindrical input converts to cartesian and back")
{
    LedPoint p = led_point_from_cylindrical(500.0f, 300.0f, 90.0f);
    CHECK(p.pos.x == doctest::Approx(0.0f).epsilon(0.001));
    CHECK(p.pos.y == doctest::Approx(300.0f));
    CHECK(p.pos.z == 500.0f);

    build({led_point_from_cylindrical(0.0f, 200.0f, 270.0f)});
    CHECK(geometry.radius(0) == doctest::Approx(200.0f));
    CHECK(geometry.angle(0) == doctest::Approx(1.5f * pi));   // [0, 2pi), not -pi/2
}

TEST_CASE("geometry: unpositioned LEDs are counted but excluded from spatial data")
{
    build({
        led_point_from_cylindrical(100.0f, 100.0f, 0.0f, PositionSource::mapped),
        LedPoint{},   // none
        led_point_from_cylindrical(900.0f, 100.0f, 0.0f, PositionSource::synthetic),
    });
    CHECK(geometry.count() == 3);
    CHECK(geometry.positioned_count() == 2);
    CHECK(geometry.mapped_count() == 1);
    CHECK_FALSE(geometry.has_position(1));
    CHECK(geometry.z_order().size() == 2);
    CHECK(geometry.bounds().min.z == 100.0f);
    CHECK(geometry.bounds().max.z == 900.0f);
    CHECK(geometry.height01(0) == 0.0f);
    CHECK(geometry.height01(2) == 1.0f);
}

TEST_CASE("geometry: z_order is ascending and in_z_range selects inclusively")
{
    build({
        led_point_from_cylindrical(500.0f, 100.0f, 0.0f),
        led_point_from_cylindrical(100.0f, 100.0f, 0.0f),
        led_point_from_cylindrical(300.0f, 100.0f, 0.0f),
        led_point_from_cylindrical(300.0f, 100.0f, 90.0f),
        led_point_from_cylindrical(700.0f, 100.0f, 0.0f),
    });
    auto order = geometry.z_order();
    REQUIRE(order.size() == 5);
    CHECK(order[0] == 1);
    CHECK(order[1] == 2);   // tie at z=300 broken by index
    CHECK(order[2] == 3);
    CHECK(order[3] == 0);
    CHECK(order[4] == 4);

    auto mid = geometry.in_z_range(300.0f, 500.0f);
    CHECK(mid.size() == 3);   // 2, 3, 0
    CHECK(geometry.in_z_range(301.0f, 499.0f).size() == 0);
    CHECK(geometry.in_z_range(-1000.0f, 1000.0f).size() == 5);
    CHECK(geometry.in_z_range(800.0f, 900.0f).empty());
    CHECK(geometry.in_z_range(500.0f, 300.0f).empty());   // inverted range
}

TEST_CASE("geometry: set past count is ignored; reset clears old data")
{
    geometry.reset(1);
    geometry.set(5, led_point_from_cylindrical(1.0f, 1.0f, 0.0f));
    geometry.finalize();
    CHECK(geometry.positioned_count() == 0);
    CHECK(geometry.bounds().max.z == 0.0f);
}
