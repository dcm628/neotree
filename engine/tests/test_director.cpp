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
uint8_t mode_of(uint8_t slot) { return dir().slot(slot).state == SlotState::empty ? no_mode : dir().slot(slot).spec.mode; }
uint32_t starts(const char *id) { return dir().stats().starts[find_mode(id)]; }

void seconds(float s) { engine.advance(static_cast<int64_t>(s * 1e6f)); }

}  // namespace

TEST_CASE("director: a cut start runs at once; a fade ramps the slot's opacity")
{
    setup();
    dir().set_slot(engine, 1, SlotSpec::of("solid"), Transition::cut);
    CHECK(dir().slot(1).state == SlotState::running);
    CHECK(engine.scene().slot(1)->opacity == 1.0f);
    CHECK(starts("solid") == 1);

    setup();
    SlotSpec s = SlotSpec::of("rainbow");
    s.life.transition_s = 2.0f;   // fade in over 1 s
    dir().set_slot(engine, 1, s, Transition::fade);
    CHECK(dir().slot(1).state == SlotState::entering);
    seconds(0.5f);
    CHECK(engine.scene().slot(1)->opacity == doctest::Approx(0.5f).epsilon(0.03));
    seconds(0.6f);
    CHECK(dir().slot(1).state == SlotState::running);
    CHECK(engine.scene().slot(1)->opacity == 1.0f);

    // Replacing it fades the old one out first.
    dir().set_slot(engine, 1, SlotSpec::of("solid"), Transition::fade);
    CHECK(dir().slot(1).state == SlotState::leaving);
    CHECK(mode_of(1) == find_mode("rainbow"));
    seconds(1.1f);
    CHECK(mode_of(1) == find_mode("solid"));
}

TEST_CASE("director: loop with repeats, then revert to the base scene")
{
    setup();
    SceneSpec base;
    base.specs[1] = SlotSpec::of("gradient");
    dir().set_base_scene(base);
    SlotSpec s = SlotSpec::of("solid");
    s.life.duration_s = 1.0f;
    s.life.policy = EndPolicy::loop;
    s.life.repeats = 3;
    s.life.transition = Transition::cut;
    dir().set_slot(engine, 1, s, Transition::cut);
    seconds(2.5f);
    CHECK(starts("solid") == 3);
    CHECK(mode_of(1) == find_mode("solid"));
    seconds(1.0f);   // third run ends: repeats used up, no chain -> revert
    CHECK(mode_of(1) == find_mode("gradient"));
    CHECK(dir().stats().ends[static_cast<int>(EndReason::duration)] == 3);
}

TEST_CASE("director: a scene's chain alternates modes forever")
{
    setup();
    SceneSpec scene;
    scene.specs[1] = SlotSpec::of("solid");
    scene.specs[1].life.duration_s = 2.0f;
    scene.specs[1].life.policy = EndPolicy::chain;
    scene.specs[1].life.next = 4;
    scene.specs[1].life.transition = Transition::cut;
    scene.specs[4] = SlotSpec::of("rainbow");
    scene.specs[4].life.duration_s = 3.0f;
    scene.specs[4].life.policy = EndPolicy::chain;
    scene.specs[4].life.next = 1;
    scene.specs[4].life.transition = Transition::cut;
    dir().apply_scene(engine, scene, Transition::cut);
    seconds(51.0f);   // 10 full rounds of 5 s
    CHECK(starts("solid") == 11);
    CHECK(starts("rainbow") == 10);
    CHECK(mode_of(1) == find_mode("solid"));
}

TEST_CASE("director: cycles end a mode; remove empties the slot")
{
    setup();
    SlotSpec s = SlotSpec::of("sweep").set("motion", 0.0f).set("speed", 2.0f);   // 2 passes/s
    s.life.cycles = 3;
    s.life.policy = EndPolicy::remove;
    s.life.transition = Transition::cut;
    dir().set_slot(engine, 1, s, Transition::cut);
    seconds(1.2f);
    CHECK(dir().slot(1).cycles_done == 2);
    CHECK(mode_of(1) == find_mode("sweep"));
    seconds(0.5f);
    CHECK(dir().slot(1).state == SlotState::empty);
    CHECK(engine.entities().size() == 0);
}

TEST_CASE("director: hold keeps running after its end condition")
{
    setup();
    SlotSpec s = SlotSpec::of("solid");
    s.life.duration_s = 1.0f;
    s.life.policy = EndPolicy::hold;
    dir().set_slot(engine, 1, s, Transition::cut);
    seconds(5.0f);
    CHECK(mode_of(1) == find_mode("solid"));
    CHECK(dir().slot(1).state == SlotState::running);
}

TEST_CASE("director: a rule's end_mode outcome overrides the policy")
{
    setup();
    SlotSpec s = SlotSpec::of("solid");
    s.life.policy = EndPolicy::remove;          // the normal ending
    s.life.outcome = 5;
    s.life.outcome_policy = EndPolicy::chain;  // outcome 5 goes to rainbow instead
    s.life.transition = Transition::cut;
    SlotSpec target = SlotSpec::of("rainbow");
    dir().set_slot(engine, 1, s, Transition::cut);
    dir().set_lifecycle(1, s.life, &target);

    Rule on_input;
    on_input.trigger = Trigger::input;
    on_input.id = 1;
    Action end;
    end.type = ActionType::end_mode;
    end.index = 5;
    on_input.then(end);
    engine.behavior().add_rule(1, on_input);
    engine.input(1, 1);
    seconds(0.1f);
    CHECK(mode_of(1) == find_mode("rainbow"));
    CHECK(dir().stats().ends[static_cast<int>(EndReason::outcome)] == 1);
}

TEST_CASE("director: reverting leaves an unchanged Canvas - and its paint - alone")
{
    setup();
    SceneSpec colors;
    colors.specs[0] = SlotSpec::of("canvas");
    dir().set_base_scene(colors);
    dir().apply_scene(engine, colors, Transition::cut);
    auto paint = engine.scene().pixels(0, 1);
    REQUIRE(!paint.empty());
    paint[3] = {10, 20, 30, 255};

    SceneSpec show = colors;
    show.specs[1] = SlotSpec::of("fireworks").set("backdrop", 0.0f);
    dir().apply_scene(engine, show, Transition::cut);
    seconds(1.0f);
    dir().revert_scene(engine);
    seconds(2.0f);
    CHECK(engine.scene().pixels(0, 1)[3].g == 20);
    CHECK(starts("canvas") == 1);
    CHECK(dir().slot(1).state == SlotState::empty);
}

TEST_CASE("director: the Canvas keeps its colors while other scenes play")
{
    setup();
    dir().apply_scene(engine, preset_at(find_preset("Colors")), Transition::cut);
    engine.scene().pixels(0, 0)[5] = {1, 2, 3, 255};
    engine.scene().pixels(0, 1)[6] = {9, 8, 7, 255};
    dir().apply_scene(engine, preset_at(find_preset("Sweep tour")), Transition::cut);
    seconds(3.0f);
    CHECK(mode_of(0) == find_mode("solid"));
    dir().apply_scene(engine, preset_at(find_preset("Colors")), Transition::cut);
    CHECK(mode_of(0) == find_mode("canvas"));
    CHECK(engine.scene().pixels(0, 0)[5].b == 3);
    CHECK(engine.scene().pixels(0, 1)[6].r == 9);
}

TEST_CASE("director: parameters change live where supported, otherwise the mode rebuilds in place")
{
    setup();
    dir().set_slot(engine, 1, SlotSpec::of("rainbow"), Transition::cut);
    ParamValue fast;
    fast.f = 0.9f;
    CHECK(dir().set_param(engine, 1, 0, fast));
    CHECK(starts("rainbow") == 1);   // live
    CHECK(engine.scene().slot(1)->layers[0].field.spin_rps == doctest::Approx(0.9f));

    SlotSpec b = SlotSpec::of("bounce").set("count", 3.0f);
    b.life.duration_s = 10.0f;
    dir().set_slot(engine, 2, b, Transition::cut);
    seconds(4.0f);
    ParamValue more;
    more.f = 7.0f;
    CHECK(dir().set_param(engine, 2, 0, more));   // count: rebuild
    CHECK(starts("bounce") == 2);
    CHECK(engine.behavior().slot_count(2) + 0 == 0);   // counts refresh next tick...
    seconds(0.02f);
    CHECK(engine.behavior().slot_count(2) == 7);
    CHECK(dir().slot(2).age_s == doctest::Approx(4.02f).epsilon(0.01));   // ...but it keeps its place
}

TEST_CASE("director: ending waits for the mode's entities to drain")
{
    setup();
    SlotSpec f = SlotSpec::of("fireworks").set("rate", 3.0f).set("backdrop", 0.0f);
    f.life.duration_s = 3.0f;
    f.life.policy = EndPolicy::remove;
    f.life.transition = Transition::cut;
    dir().set_slot(engine, 1, f, Transition::cut);
    seconds(3.05f);
    CHECK(dir().slot(1).state == SlotState::ending);   // emitters off, sparks still falling
    seconds(4.0f);
    CHECK(dir().slot(1).state == SlotState::empty);
    CHECK(dir().stats().drain_timeouts == 0);
}

TEST_CASE("director: describe output is well formed")
{
    setup();
    std::vector<char> buf(8192);
    size_t n = describe_modes(buf.data(), buf.size());
    std::string modes(buf.data(), n);
    CHECK(modes.find("\"id\":\"snow\"") != std::string::npos);
    CHECK(modes.find("\"presets\":[") != std::string::npos);
    int depth = 0;
    for (char ch : modes)
    {
        depth += ch == '{' || ch == '[' ? 1 : ch == '}' || ch == ']' ? -1 : 0;
    }
    CHECK(depth == 0);
    MESSAGE("describe_modes: " << n << " bytes");
    CHECK(n < 5000);   // fits the tree's 6 KB DESCRIBE reply frame with room to grow

    dir().apply_scene(engine, preset_at(find_preset("Holiday show")), Transition::cut);
    n = dir().describe_state(buf.data(), buf.size());
    std::string state(buf.data(), n);
    CHECK(state.find("\"scene\":\"Holiday show\"") != std::string::npos);
    CHECK(state.find("\"mode\":\"fireworks\"") != std::string::npos);
    CHECK(n < 1500);
}

TEST_CASE("presets: the holiday show keeps alternating over hours of simulated time")
{
    setup();
    dir().set_base_scene(preset_at(0));
    dir().apply_scene(engine, preset_at(find_preset("Holiday show")), Transition::cut);
    for (int minute = 0; minute < 6 * 60; minute++)   // 6 hours
    {
        seconds(60.0f);
        REQUIRE(dir().slot(1).state != SlotState::empty);
    }
    // 30 s of fireworks + 45 s of snow (+ fades and draining) per round.
    const uint32_t fireworks = starts("fireworks");
    const uint32_t snow = starts("snow");
    CHECK(fireworks > 200);   // ~235: rounds are ~92 s with fades and the snow's 14 s drain
    CHECK(snow > 200);
    CHECK((fireworks == snow || fireworks == snow + 1));
    CHECK(engine.behavior().stats().events_dropped == 0);
}

TEST_CASE("presets: every preset starts and runs")
{
    for (uint8_t p = 0; p < preset_count(); p++)
    {
        setup();
        CHECK(find_preset(preset_at(p).name) == p);
        dir().apply_scene(engine, preset_at(p), Transition::cut);
        std::vector<Rgb> out(geometry.count());
        for (int f = 0; f < 600; f++)
        {
            engine.advance(16'667);
            engine.render(out);
        }
        CHECK(engine.stats().spawns_failed == 0);
    }
}
