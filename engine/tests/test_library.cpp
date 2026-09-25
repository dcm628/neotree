#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include "doctest/doctest.h"
#include "neotree/engine.hpp"

using namespace neotree;

namespace {

LedGeometry geometry;
Engine engine;

void setup()
{
    geometry.reset(21);
    for (uint16_t i = 0; i <= 20; i++)
    {
        geometry.set(i, led_point_from_cylindrical(100.0f * i, 300.0f, 30.0f * i));
    }
    geometry.finalize();
    EngineConfig config;
    config.tick_hz = 100;
    config.max_ticks_per_advance = 0;
    engine.init(geometry, config);
}

Director &dir() { return engine.director(); }
Library &lib() { return engine.director().library(); }
void seconds(float s) { engine.advance(static_cast<int64_t>(s * 1e6f)); }
uint8_t mode_of(uint8_t slot) { return dir().slot(slot).state == SlotState::empty ? no_mode : dir().slot(slot).spec.mode; }

// Stored-form buffers live here, not on the stack (as on the tree).
std::vector<uint8_t> stored(16384);

Show make_show(const char *name, std::initializer_list<std::pair<const char *, uint16_t>> entries)
{
    Show s;
    std::snprintf(s.name, sizeof(s.name), "%s", name);
    for (const auto &e : entries)
    {
        std::snprintf(s.entries[s.count].preset, name_size, "%s", e.first);
        s.entries[s.count++].duration_s = e.second;
    }
    return s;
}

}  // namespace

TEST_CASE("library: user presets are added, replaced by name and deleted; built-ins are protected")
{
    setup();
    const uint8_t builtins = lib().preset_count();
    const uint32_t rev = lib().revision();
    SceneSpec a;
    a.specs[0] = SlotSpec::of("solid");
    uint8_t i = lib().save_preset(a, "  Mine  ");
    CHECK(i == builtins);
    CHECK(std::strcmp(lib().preset(i).name, "Mine") == 0);
    CHECK(lib().preset_is_user(i));
    CHECK(lib().revision() > rev);

    a.specs[0] = SlotSpec::of("rainbow");
    CHECK(lib().save_preset(a, "Mine") == i);   // same name: replaced, not added
    CHECK(lib().preset_count() == builtins + 1);
    CHECK(lib().preset(i).specs[0].mode == find_mode("rainbow"));

    CHECK(lib().save_preset(a, "Colors") == no_index);   // a built-in's name
    CHECK(lib().save_preset(a, "   ") == no_index);
    CHECK(!lib().delete_preset(0));
    for (int k = 1; k < max_user_presets; k++)
    {
        CHECK(lib().save_preset(a, ("P" + std::to_string(k)).c_str()) != no_index);
    }
    CHECK(lib().save_preset(a, "One too many") == no_index);
    CHECK(lib().delete_preset(i));
    CHECK(lib().find_preset("Mine") == no_index);
    CHECK(lib().preset_count() == builtins + max_user_presets - 1);
}

TEST_CASE("library: the stored form round-trips everything the user made")
{
    setup();
    SceneSpec scene;
    scene.duration_s = 90.0f;
    scene.policy = EndPolicy::loop;
    scene.specs[0] = SlotSpec::of("gradient").set("top", to_rgb(10, 20, 30));
    scene.specs[1] = SlotSpec::of("snow").set("rate", 7.0f).set("backdrop", 0.0f);
    scene.specs[1].opacity = 0.5f;
    scene.specs[1].life.duration_s = 12.5f;
    scene.specs[1].life.policy = EndPolicy::chain;
    scene.specs[1].life.next = 5;
    scene.specs[1].life.transition = Transition::cut;
    scene.specs[5] = SlotSpec::of("fireworks").set("sparks", 20.0f);
    REQUIRE(lib().save_preset(scene, "Winter \"quoted\"") != no_index);
    lib().set_base(scene);
    REQUIRE(lib().save_show(make_show("Night", {{"Winter \"quoted\"", 60}, {"Colors", 0}})) != no_index);
    REQUIRE(lib().set_boot_show(lib().find_show("Night")));

    const size_t n = lib().save(stored.data(), stored.size());
    REQUIRE(n > 0);
    MESSAGE("stored library: " << n << " bytes");

    setup();   // a fresh engine: built-ins only
    CHECK(lib().preset_count() == preset_count());
    REQUIRE(lib().load(stored.data(), n));
    const uint8_t p = lib().find_preset("Winter \"quoted\"");
    REQUIRE(p != no_index);
    const SceneSpec &back = lib().preset(p);
    CHECK(back.duration_s == 90.0f);
    CHECK(back.policy == EndPolicy::loop);
    CHECK(back.specs[0].mode == find_mode("gradient"));
    CHECK(unit_to_byte(back.specs[0].params[1].c.g) == 20);
    CHECK(back.specs[1].params[0].f == 7.0f);
    CHECK(back.specs[1].opacity == 0.5f);
    CHECK(back.specs[1].life.duration_s == 12.5f);
    CHECK(back.specs[1].life.next == 5);
    CHECK(back.specs[1].life.transition == Transition::cut);
    CHECK(back.specs[5].mode == find_mode("fireworks"));
    CHECK(back.specs[2].mode == no_mode);
    CHECK(lib().base_is_custom());
    CHECK(lib().base().specs[1].mode == find_mode("snow"));
    const uint8_t night = lib().find_show("Night");
    REQUIRE(night != no_index);
    CHECK(lib().boot_show() == night);
    CHECK(lib().show(night).count == 2);
    CHECK(lib().show(night).entries[0].duration_s == 60);
}

TEST_CASE("library: stored scenes survive modes and parameters changing")
{
    setup();
    SceneSpec scene;
    scene.specs[0] = SlotSpec::of("rainbow").set("speed", 0.5f);
    scene.specs[1] = SlotSpec::of("snow");
    REQUIRE(lib().save_preset(scene, "Old") != no_index);
    size_t n = lib().save(stored.data(), stored.size());
    REQUIRE(n > 0);

    // Pretend a later firmware renamed the snow mode and rainbow's "speed"
    // parameter (same lengths, so the stored form stays valid).
    auto rename = [&](const char *from, const char *to) {
        const size_t len = std::strlen(from);
        for (size_t k = 0; k + len <= n; k++)
        {
            if (stored[k] == len && std::memcmp(&stored[k + 1], from, len) == 0)
            {
                std::memcpy(&stored[k + 1], to, len);
                return true;
            }
        }
        return false;
    };
    REQUIRE(rename("snow", "sn0w"));
    REQUIRE(rename("speed", "sp33d"));

    setup();
    REQUIRE(lib().load(stored.data(), n));
    const SceneSpec &back = lib().preset(lib().find_preset("Old"));
    CHECK(back.specs[0].mode == find_mode("rainbow"));
    CHECK(back.specs[0].params[0].f == doctest::Approx(0.2f));   // unknown parameter: the default instead
    CHECK(back.specs[1].mode == no_mode);                        // unknown mode: the slot is empty
}

TEST_CASE("library: damaged stored data is rejected and leaves the built-ins")
{
    setup();
    SceneSpec scene;
    scene.specs[0] = SlotSpec::of("solid");
    lib().save_preset(scene, "Mine");
    const size_t n = lib().save(stored.data(), stored.size());
    REQUIRE(n > 0);
    for (size_t cut = 0; cut < n; cut++)
    {
        CHECK(!lib().load(stored.data(), cut));
    }
    std::vector<uint8_t> bad(stored.begin(), stored.begin() + static_cast<std::ptrdiff_t>(n));
    bad[0] = 99;   // a version from the future
    CHECK(!lib().load(bad.data(), bad.size()));
    CHECK(lib().preset_count() == preset_count());
    CHECK(!lib().base_is_custom());
    CHECK(lib().load(stored.data(), n));
    CHECK(lib().find_preset("Mine") != no_index);
    CHECK(lib().save(stored.data(), 10) == 0);   // too small to hold it
}

TEST_CASE("library: a full library fits the tree's store and its description fits a reply")
{
    setup();
    for (int k = 0; k < max_user_presets; k++)
    {
        SceneSpec scene;
        for (uint8_t s = 0; s < max_specs; s++)
        {
            scene.specs[s] = SlotSpec::of("sweep");   // five parameters each
        }
        REQUIRE(lib().save_preset(scene, ("Preset number " + std::to_string(k)).c_str()) != no_index);
    }
    for (int k = 0; k < max_user_shows; k++)
    {
        Show show = make_show(("Show number " + std::to_string(k)).c_str(), {});
        for (uint8_t e = 0; e < max_show_entries; e++)
        {
            std::snprintf(show.entries[e].preset, name_size, "Preset number %d", e % 8);
            show.entries[e].duration_s = 600;
        }
        show.count = max_show_entries;
        REQUIRE(lib().save_show(show) != no_index);
    }
    SceneSpec base;
    for (uint8_t s = 0; s < max_specs; s++)
    {
        base.specs[s] = SlotSpec::of("sweep");
    }
    lib().set_base(base);
    const size_t n = lib().save(stored.data(), stored.size());
    MESSAGE("largest stored library: " << n << " bytes");
    CHECK(n > 0);
    CHECK(n <= 16384 - 16);   // the tree's store: 4 flash sectors less its header

    std::vector<char> json(8192);
    const size_t len = lib().describe(json.data(), json.size());
    MESSAGE("largest library description: " << len << " bytes");
    CHECK(len < 3600);   // one 4 KB reply frame
    std::string s(json.data(), len);
    int depth = 0;
    for (char ch : s)
    {
        depth += ch == '{' || ch == '[' ? 1 : ch == '}' || ch == ']' ? -1 : 0;
    }
    CHECK(depth == 0);
}

TEST_CASE("library: names are cleaned and escaped in JSON")
{
    setup();
    char name[name_size];
    CHECK(copy_name(name, "  a\tb\"c\\  ", 12));
    CHECK(std::string(name) == "ab\"c\\");
    CHECK(!copy_name(name, "\x01\x02 ", 3));
    CHECK(copy_name(name, "abcdefghijklmnopqrstuvwxyz", 26));
    CHECK(std::strlen(name) == name_size - 1);

    SceneSpec scene;
    lib().save_preset(scene, "say \"hi\"");
    std::vector<char> json(4096);
    std::string s(json.data(), lib().describe(json.data(), json.size()));
    CHECK(s.find("\"n\":\"say \\\"hi\\\"\",\"u\":1") != std::string::npos);
}

TEST_CASE("director: capturing the live scene keeps live parameters and protocol chain targets")
{
    setup();
    dir().apply_scene(engine, preset_at(find_preset("Holiday show")), Transition::cut);
    ParamValue v;
    v.c = to_rgb(200, 0, 0);
    REQUIRE(dir().set_param(engine, 0, 0, v));
    SlotSpec lighthouse = SlotSpec::of("lighthouse");
    dir().set_slot(engine, 2, SlotSpec::of("orbit"), Transition::cut);
    Lifecycle life;
    life.duration_s = 20.0f;
    life.policy = EndPolicy::chain;
    dir().set_lifecycle(2, life, &lighthouse);

    SceneSpec captured;
    dir().capture_scene(captured, "Captured");
    CHECK(std::string(captured.name) == "Captured");
    CHECK(unit_to_byte(captured.specs[0].params[0].c.r) == 200);
    CHECK(captured.specs[1].mode == find_mode("fireworks"));
    CHECK(captured.specs[4].mode == find_mode("snow"));   // the preset's own chain target is kept
    CHECK(captured.specs[2].mode == find_mode("orbit"));
    const uint8_t next = captured.specs[2].life.next;
    REQUIRE(next >= max_slots);
    REQUIRE(next < max_specs);
    CHECK(captured.specs[next].mode == find_mode("lighthouse"));

    // Played back on a fresh engine, it chains the same way.
    setup();
    dir().apply_scene(engine, captured, Transition::cut);
    CHECK(mode_of(2) == find_mode("orbit"));
    seconds(22.0f);
    CHECK(mode_of(2) == find_mode("lighthouse"));
}

TEST_CASE("shows: entries play in order for their durations, then loop")
{
    setup();
    lib().save_show(make_show("Test", {{"Snow on rainbow", 10}, {"Sweep tour", 20}, {"Colors", 5}}));
    const uint8_t show = lib().find_show("Test");
    REQUIRE(dir().play_show(engine, show));
    CHECK(std::string(dir().show_status().preset) == "Snow on rainbow");
    CHECK(mode_of(1) == find_mode("snow"));
    seconds(10.5f);
    CHECK(std::string(dir().show_status().preset) == "Sweep tour");
    seconds(20.0f);
    CHECK(std::string(dir().show_status().preset) == "Colors");
    seconds(5.0f);
    CHECK(dir().show_status().rounds == 1);
    CHECK(std::string(dir().show_status().preset) == "Snow on rainbow");
    CHECK(dir().stats().show_entries == 4);

    // Editing the scene doesn't stop it; its next entry replaces the edit.
    dir().set_slot(engine, 3, SlotSpec::of("orbit"), Transition::cut);
    CHECK(dir().show_status().playing);
    seconds(11.0f);   // the next entry at 10 s, and its 0.75 s fade-out of the orbit
    CHECK(std::string(dir().show_status().preset) == "Sweep tour");
    CHECK(mode_of(3) == no_mode);

    dir().stop_show();
    CHECK(!dir().show_status().playing);
    const uint8_t sweep = mode_of(1);
    seconds(60.0f);
    CHECK(mode_of(1) == sweep);   // the scene stays as it was
}

TEST_CASE("shows: a show that doesn't loop reverts to the base scene; missing presets are skipped")
{
    setup();
    SceneSpec mine;
    mine.specs[0] = SlotSpec::of("solid");
    lib().save_preset(mine, "Doomed");
    Show once = make_show("Once", {{"Doomed", 5}, {"Rainbow nowhere", 5}, {"Snow on rainbow", 5}});
    once.loop = false;
    lib().save_show(once);
    lib().delete_preset(lib().find_preset("Doomed"));

    REQUIRE(dir().play_show(engine, lib().find_show("Once")));
    CHECK(std::string(dir().show_status().preset) == "Snow on rainbow");
    CHECK(dir().stats().show_skips == 2);
    seconds(6.5f);   // over at 5 s, then the fade back to base
    CHECK(!dir().show_status().playing);
    CHECK(mode_of(0) == find_mode("canvas"));   // the base scene: Colors
    CHECK(mode_of(1) == no_mode);

    Show none = make_show("Nothing", {{"Gone", 5}});
    lib().save_show(none);
    CHECK(!dir().play_show(engine, lib().find_show("Nothing")));
}

TEST_CASE("shows: shuffle plays every entry each round, never the same twice in a row")
{
    setup();
    Show s = make_show("Mix", {{"Colors", 1}, {"Snow on rainbow", 1}, {"Holiday show", 1}, {"Sweep tour", 1},
                               {"Fireworks finale", 1}});
    s.shuffle = true;
    lib().save_show(s);
    REQUIRE(dir().play_show(engine, lib().find_show("Mix")));
    seconds(0.5f);   // look mid-entry, never on a boundary
    std::string previous;
    bool orders_differ = false;
    std::string first_round;
    for (int round = 0; round < 40; round++)
    {
        std::string seen;
        for (int e = 0; e < 5; e++)
        {
            const std::string preset = dir().show_status().preset;
            CHECK(preset != previous);
            CHECK(seen.find(preset) == std::string::npos);
            seen += preset + "|";
            previous = preset;
            seconds(1.0f);
        }
        if (round == 0)
        {
            first_round = seen;
        }
        else if (seen != first_round)
        {
            orders_differ = true;
        }
    }
    CHECK(orders_differ);
}

TEST_CASE("shows: the built-in evening show keeps going round (6 h of simulated time)")
{
    setup();
    REQUIRE(dir().play_show(engine, lib().find_show("Holiday evening")));
    std::vector<Rgb> out(geometry.count());
    for (int hour = 0; hour < 6; hour++)
    {
        seconds(3600.0f);
        REQUIRE(dir().show_status().playing);
    }
    engine.render(out);
    const uint32_t round_s = 240 + 600 + 180 + 90 + 210;
    const uint32_t expected = 6 * 3600 / round_s;
    MESSAGE("rounds in 6 h: " << dir().stats().show_rounds);
    CHECK(dir().stats().show_rounds >= expected - 1);
    CHECK(dir().stats().show_rounds <= expected);
    CHECK(engine.behavior().stats().events_dropped == 0);
    CHECK(engine.stats().spawns_failed == 0);
}

TEST_CASE("director: the revision moves on changes, not as time passes")
{
    setup();
    dir().apply_scene(engine, preset_at(0), Transition::cut);
    uint32_t r = dir().revision();
    seconds(5.0f);
    CHECK(dir().revision() == r);
    dir().set_slot(engine, 1, SlotSpec::of("snow"), Transition::cut);
    CHECK(dir().revision() > r);
    r = dir().revision();
    ParamValue v;
    v.f = 5.0f;
    dir().set_param(engine, 1, 0, v);
    CHECK(dir().revision() > r);

    std::vector<char> buf(4096);
    std::string state(buf.data(), dir().describe_state(buf.data(), buf.size()));
    CHECK(state.find("\"rev\":") == 1);
    CHECK(state.find("\"show\":null") != std::string::npos);
    dir().play_show(engine, lib().find_show("Holiday evening"));
    state.assign(buf.data(), dir().describe_state(buf.data(), buf.size()));
    CHECK(state.find("\"show\":{\"n\":\"Holiday evening\",\"entry\":\"Snow on rainbow\",\"pos\":0,\"of\":5") !=
          std::string::npos);
}
