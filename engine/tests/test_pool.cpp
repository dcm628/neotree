#include "doctest/doctest.h"
#include "neotree/pool.hpp"

using neotree::Handle;
using neotree::Pool;

struct Thing
{
    int value = 0;
};

TEST_CASE("pool: create, get, destroy")
{
    Pool<Thing, 4> pool;
    Handle h = pool.create();
    REQUIRE(h.valid());
    CHECK(pool.size() == 1);
    pool.get(h)->value = 5;
    CHECK(pool.get(h)->value == 5);
    CHECK(pool.destroy(h));
    CHECK(pool.get(h) == nullptr);
    CHECK_FALSE(pool.destroy(h));   // double free is rejected
    CHECK(pool.size() == 0);
}

TEST_CASE("pool: a stale handle never reaches the slot's new occupant")
{
    Pool<Thing, 1> pool;
    Handle old_h = pool.create();
    pool.destroy(old_h);
    Handle new_h = pool.create();
    CHECK(new_h.index == old_h.index);
    CHECK(new_h.generation != old_h.generation);
    CHECK(pool.get(old_h) == nullptr);
    CHECK(pool.get(new_h) != nullptr);
}

TEST_CASE("pool: full pool returns an invalid handle; freeing makes room")
{
    Pool<Thing, 3> pool;
    Handle hs[3];
    for (auto &h : hs)
    {
        h = pool.create();
        REQUIRE(h.valid());
    }
    CHECK(pool.full());
    CHECK_FALSE(pool.create().valid());
    pool.destroy(hs[1]);
    CHECK(pool.create().valid());
}

TEST_CASE("pool: new objects start value-initialized")
{
    Pool<Thing, 2> pool;
    Handle h = pool.create();
    pool.get(h)->value = 9;
    pool.destroy(h);
    CHECK(pool.get(pool.create())->value == 0);
}

TEST_CASE("pool: clear invalidates every handle")
{
    Pool<Thing, 2> pool;
    Handle a = pool.create();
    Handle b = pool.create();
    pool.clear();
    CHECK(pool.size() == 0);
    CHECK(pool.get(a) == nullptr);
    CHECK(pool.get(b) == nullptr);
}

TEST_CASE("pool: iteration by slot sees exactly the live objects")
{
    Pool<Thing, 5> pool;
    Handle hs[5];
    for (int i = 0; i < 5; i++)
    {
        hs[i] = pool.create();
        pool.get(hs[i])->value = i;
    }
    pool.destroy(hs[1]);
    pool.destroy(hs[3]);
    int sum = 0, n = 0;
    for (uint16_t i = 0; i < pool.capacity; i++)
    {
        if (pool.alive(i))
        {
            sum += pool.item(i).value;
            n++;
            CHECK(pool.get(pool.handle_at(i)) == &pool.item(i));
        }
    }
    CHECK(n == 3);
    CHECK(sum == 0 + 2 + 4);
}
