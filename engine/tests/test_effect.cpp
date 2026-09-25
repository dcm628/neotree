#include <cstring>
#include <string>
#include <vector>

#include "doctest/doctest.h"
#include "neotree/effect.hpp"
#include "neotree/engine.hpp"

using namespace neotree;

namespace {

LedGeometry geometry;
Engine engine;

void setup()
{
    geometry.reset(60);
    for (uint16_t i = 0; i < 60; i++)
    {
        geometry.set(i, led_point_from_cylindrical(30.0f * i, 500.0f - 7.0f * i, 47.0f * i));
    }
    geometry.finalize();
    EngineConfig config;
    config.tick_hz = 100;
    config.max_ticks_per_advance = 0;
    engine.init(geometry, config);
    engine.director().library().clear_effects();
}

Director &dir() { return engine.director(); }
void seconds(float s) { engine.advance(static_cast<int64_t>(s * 1e6f)); }

uint16_t alive_in(uint8_t slot)
{
    uint16_t n = 0;
    const EntityPool &pool = engine.entities();
    for (uint16_t k = 0; k < pool.capacity; k++)
    {
        n += pool.alive(k) && pool.item(k).slot == slot;
    }
    return n;
}

FieldValue num(float f)
{
    FieldValue v;
    v.f = f;
    return v;
}

FieldValue col(float r, float g, float b)
{
    FieldValue v;
    v.c = {r, g, b};
    return v;
}

std::vector<Rgb8> frame()
{
    std::vector<Rgb8> b(geometry.count());
    engine.render_bytes(b);
    return b;
}

// Saves, loads and saves again: the two stored forms must match.
size_t check_round_trip(const Effect &e)
{
    static uint8_t a[max_effect_bytes], b[max_effect_bytes];
    const size_t la = effect_save(e, a, sizeof(a));
    REQUIRE(la > 0);
    Effect &back = effect_scratch();
    REQUIRE(effect_load(back, a, la));
    const size_t lb = effect_save(back, b, sizeof(b));
    CHECK(lb == la);
    CHECK(std::memcmp(a, b, la) == 0);
    CHECK(std::strcmp(back.name, e.name) == 0);
    return la;
}

constexpr EffectSection things = EffectSection::things;

}  // namespace

TEST_CASE("effect schema: ids unique, choices match ranges, shown_by points back")
{
    for (uint8_t s = 0; s < static_cast<uint8_t>(EffectSection::count); s++)
    {
        const EffectSectionInfo &info = effect_section(static_cast<EffectSection>(s));
        CAPTURE(info.id);
        for (uint8_t f = 0; f < info.field_count; f++)
        {
            const EffectField &field = info.fields[f];
            CAPTURE(field.id);
            for (uint8_t g = 0; g < f; g++)
            {
                CHECK(std::strcmp(info.fields[g].id, field.id) != 0);
            }
            if (field.type == FieldType::choice)
            {
                REQUIRE(field.choices != nullptr);
                int options = 1;
                for (const char *c = field.choices; *c; c++)
                {
                    options += *c == '|';
                }
                CHECK(options == static_cast<int>(field.max) + 1);
            }
            if (field.shown_by >= 0)
            {
                CHECK(field.shown_by < f);
                const FieldType by = info.fields[field.shown_by].type;
                CHECK((by == FieldType::choice || by == FieldType::toggle));
            }
        }
        char json[6144];
        const size_t n = effect_schema_json(static_cast<EffectSection>(s), json, sizeof(json));
        CHECK(n < sizeof(json) - 1);
        CHECK(json[n - 1] == '}');
    }
}

TEST_CASE("effect: a new one runs - things fall from the top and live")
{
    setup();
    REQUIRE(dir().edit_effect(engine, 2, no_mode));
    CHECK(dir().draft_slot() == 2);
    CHECK(dir().slot(2).spec.mode == draft_mode);
    CHECK(std::strcmp(mode_at(draft_mode)->id, "fx.draft") == 0);
    seconds(3.0f);
    CHECK(alive_in(2) >= 3);
    bool lit = false;
    for (const Rgb8 &c : frame())
    {
        lit = lit || c.b > 40;
    }
    CHECK(lit);
    check_round_trip(dir().draft());
}

TEST_CASE("effect: edits apply live - to the thing and what's made from it")
{
    setup();
    REQUIRE(dir().edit_effect(engine, 2, no_mode));
    seconds(2.0f);
    const uint16_t before = alive_in(2);
    REQUIRE(before > 0);

    CHECK(dir().edit_field(engine, things, 0, 5, col(1.0f, 0.0f, 0.0f)));   // color
    CHECK(dir().edit_field(engine, things, 0, 1, num(150.0f)));             // size
    CHECK(alive_in(2) == before);                                         // not set up again
    const EntityPool &pool = engine.entities();
    int checked = 0;
    for (uint16_t k = 0; k < pool.capacity; k++)
    {
        if (pool.alive(k) && pool.item(k).slot == 2)
        {
            CHECK(pool.item(k).color.r == 1.0f);
            CHECK(pool.item(k).color.b == 0.0f);
            CHECK(pool.item(k).size == 150.0f);
            checked++;
        }
    }
    CHECK(checked == before);
    CHECK(engine.behavior().template_at(2, 0)->size == 150.0f);

    // The source off: nothing new appears (and what's there lives out).
    CHECK(dir().edit_field(engine, EffectSection::sources, 0, 2, num(0.0f)));
    CHECK_FALSE(engine.behavior().emitter(2, 0)->active);

    // The backdrop's color, live.
    CHECK(dir().edit_field(engine, EffectSection::layers, 0, 1, col(0.0f, 0.5f, 0.0f)));
    CHECK(engine.scene().layer(2, 0)->color.g == 0.5f);

    // Forces.
    CHECK(dir().edit_field(engine, EffectSection::settings, 0, 1, num(1.0f)));
    CHECK(dir().edit_field(engine, EffectSection::settings, 0, 2, num(500.0f)));
    CHECK(engine.forces().gravity.z == -500.0f);
}

TEST_CASE("effect: built from nothing with a rule - a timer that makes bursts")
{
    setup();
    REQUIRE(dir().edit_effect(engine, 1, no_mode));
    // No source: things come only from the rule.
    REQUIRE(dir().edit_item(engine, 1, EffectSection::sources, 0));
    REQUIRE(dir().edit_item(engine, 0, EffectSection::rules, 0));
    CHECK(dir().edit_field(engine, EffectSection::rules, 0, 1, num(static_cast<float>(Trigger::timer))));
    CHECK(dir().edit_field(engine, EffectSection::rules, 0, 6, num(0.5f)));   // after
    CHECK(dir().edit_field(engine, EffectSection::rules, 0, 7, num(1.0f)));   // then every
    REQUIRE(dir().edit_item(engine, 0, EffectSection::actions, 0));
    const uint8_t act = 0 * max_actions_per_rule + 0;
    CHECK(dir().edit_field(engine, EffectSection::actions, act, 0, num(0.0f)));   // make things
    CHECK(dir().edit_field(engine, EffectSection::actions, act, 8, num(4.0f)));   // 4 of them
    CHECK(dir().edit_field(engine, EffectSection::actions, act, 9, num(4.0f)));   // anywhere inside
    CHECK(alive_in(1) == 0);
    seconds(0.6f);
    CHECK(alive_in(1) == 4);
    seconds(1.0f);
    CHECK(alive_in(1) == 8);

    // Edits to the rule keep its timer going.
    CHECK(dir().edit_field(engine, EffectSection::actions, act, 8, num(2.0f)));
    seconds(1.0f);
    CHECK(alive_in(1) == 10);

    check_round_trip(dir().draft());

    // What it starts with.
    REQUIRE(dir().edit_item(engine, 0, EffectSection::starts, 0));
    CHECK(dir().edit_field(engine, EffectSection::starts, 0, 1, num(7.0f)));
    CHECK(alive_in(1) == 7);   // set up again, with its 7
}

TEST_CASE("effect: things meeting - groups and what happens")
{
    setup();
    REQUIRE(dir().edit_effect(engine, 1, no_mode));
    REQUIRE(dir().edit_item(engine, 0, EffectSection::meets, 0));   // A meets A: bounce
    CHECK(engine.behavior().response(1, 0, 0) == Response::bounce);
    CHECK(dir().edit_field(engine, EffectSection::meets, 0, 2, num(static_cast<float>(Response::destroy_both))));
    CHECK(engine.behavior().response(1, 0, 0) == Response::destroy_both);
    REQUIRE(dir().edit_item(engine, 1, EffectSection::meets, 0));
    CHECK(engine.behavior().response(1, 0, 0) == Response::ignore);
}

TEST_CASE("effect: removing a thing renumbers what refers to the ones after it")
{
    Effect &e = effect_scratch();
    effect_starter(e);
    REQUIRE(effect_add(e, things, 0));
    REQUIRE(effect_add(e, things, 0));
    e.sources[0].template_index = 2;
    REQUIRE(effect_add(e, EffectSection::rules, 0));
    REQUIRE(effect_add(e, EffectSection::actions, 0));
    e.rules[0].actions[0].index = 2;   // make thing 3
    REQUIRE(effect_remove(e, things, 1));
    CHECK(e.thing_count == 2);
    CHECK(e.sources[0].template_index == 1);
    CHECK(e.rules[0].actions[0].index == 1);
    REQUIRE(effect_duplicate(e, EffectSection::actions, 0));
    CHECK(e.rules[0].action_count == 2);
    CHECK_FALSE(effect_item_exists(e, EffectSection::actions, 2));
    CHECK(effect_item_exists(e, EffectSection::actions, 1));
}

TEST_CASE("effect: a copy of every built-in stores, and runs")
{
    for (uint8_t m = 0; m < mode_count(); m++)
    {
        const std::string id = mode_at(m)->id;
        CAPTURE(id);
        setup();
        REQUIRE(dir().edit_effect(engine, 1, m));
        CHECK(std::strcmp(dir().draft().name, mode_at(m)->name) == 0);
        const size_t len = check_round_trip(dir().draft());
        CHECK(len <= max_effect_bytes);
        for (uint8_t s = 0; s < static_cast<uint8_t>(EffectSection::count); s++)
        {
            char json[6144];
            const size_t n = effect_section_json(dir().draft(), static_cast<EffectSection>(s), json, sizeof(json));
            CHECK(n < sizeof(json) - 1);
        }
        seconds(2.0f);
        CHECK(dir().slot(1).spec.mode == draft_mode);
    }
}

TEST_CASE("effect: a copy of a field mode draws the same as the mode")
{
    for (const char *id : {"solid", "gradient", "rainbow", "lighthouse"})
    {
        CAPTURE(id);
        setup();
        dir().set_slot(engine, 1, SlotSpec::of(id), Transition::cut);
        seconds(0.5f);
        const std::vector<Rgb8> want = frame();
        setup();
        REQUIRE(dir().edit_effect(engine, 1, find_mode(id)));
        seconds(0.5f);
        const std::vector<Rgb8> got = frame();
        for (size_t i = 0; i < want.size(); i++)
        {
            CHECK(got[i].r == want[i].r);
            CHECK(got[i].g == want[i].g);
            CHECK(got[i].b == want[i].b);
        }
    }
}

TEST_CASE("effect: saved, it's a mode - listed, by id, in scenes - and deleted, gone")
{
    setup();
    REQUIRE(dir().edit_effect(engine, 1, find_mode("bounce")));
    const uint8_t k = dir().save_draft("  Balls ");
    REQUIRE(k == 0);
    CHECK(std::strcmp(dir().draft().name, "Balls") == 0);
    CHECK(dir().library().find_effect("Balls") == 0);
    CHECK(find_mode("fx:Balls") == effect_mode(0));
    CHECK(std::strcmp(mode_at(effect_mode(0))->name, "Balls") == 0);

    static char json[6144];
    dir().library().describe(json, sizeof(json));
    CHECK(std::string(json).find("\"fx\":[{\"n\":\"Balls\",\"k\":0,\"i\":49}]") != std::string::npos);

    // Saving under the same name replaces it; another name takes a new place.
    CHECK(dir().save_draft("Balls") == 0);
    CHECK(dir().save_draft("More balls") == 1);

    // It runs by itself.
    dir().set_slot(engine, 2, SlotSpec::of("fx:Balls"), Transition::cut);
    CHECK(dir().slot(2).state == SlotState::running);
    seconds(0.5f);
    CHECK(alive_in(2) > 0);

    // A scene holding it stores it by id.
    static SceneSpec scene;
    dir().capture_scene(scene, "With balls");
    REQUIRE(dir().library().save_preset(scene, "With balls") != no_index);
    static uint8_t lib[16384], fx[16384];
    const size_t lib_len = dir().library().save(lib, sizeof(lib));
    const size_t fx_len = dir().library().save_effects(fx, sizeof(fx));
    REQUIRE(lib_len > 0);
    REQUIRE(fx_len > 0);

    // As at startup: effects first, then the rest (whose scenes name them).
    setup();
    CHECK(find_mode("fx:Balls") == no_mode);
    REQUIRE(dir().library().load_effects(fx, fx_len));
    REQUIRE(dir().library().load(lib, lib_len));
    CHECK(find_mode("fx:Balls") == effect_mode(0));
    CHECK(find_mode("fx:More balls") == effect_mode(1));
    const uint8_t p = dir().library().find_preset("With balls");
    REQUIRE(p != no_index);
    CHECK(dir().library().preset(p).specs[2].mode == effect_mode(0));

    // Opening it to edit keeps its name.
    REQUIRE(dir().edit_effect(engine, 3, effect_mode(1)));
    CHECK(std::strcmp(dir().draft().name, "More balls") == 0);

    CHECK(dir().library().delete_effect(0));
    CHECK(mode_at(effect_mode(0)) == nullptr);
    CHECK(find_mode("fx:Balls") == no_mode);
    CHECK(find_mode("fx:More balls") == effect_mode(1));   // stays where it was

    // Malformed stored effects load as none.
    fx[5] ^= 0x5A;
    CHECK_FALSE(dir().library().load_effects(fx, fx_len));
    CHECK_FALSE(dir().library().effect_used(1));
}
