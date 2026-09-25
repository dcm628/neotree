// neotree_sim - headless engine runner. Steps the engine back to back with no
// sleeping, so it covers simulation time far faster than real time
// (docs/RENDERER.md section 11), and reports how fast it went plus the
// engine's own stats.
//
//   neotree_sim [--scene NAME] [--duration 8h] [--fps 60] [--tick-hz 120]
//               [--seed 1] [--render-every N] [--positions FILE] [--quiet]
//
// --render-every N renders one frame in N (0 = never): runs that only care
// about lifecycles and rules can skip most of the rendering cost.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "neotree/engine.hpp"
#include "demo_scenes.hpp"
#include "positions_csv.hpp"

namespace {

void log_to_stderr(const char *text) { std::fprintf(stderr, "%s\n", text); }

void usage()
{
    std::fprintf(stderr,
                 "usage: neotree_sim [--scene NAME] [--duration T] [--fps N] [--tick-hz N] [--seed N]\n"
                 "                   [--render-every N] [--positions FILE] [--quiet]\n"
                 "  T accepts s/m/h/d suffixes (default 60s)\n"
                 "  scenes: %s (default empty)\n",
                 neotree::sim::demo_scene_names().c_str());
}

std::string format_time(double s)
{
    long long total_ms = static_cast<long long>(s * 1000.0 + 0.5);
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld.%03lld", total_ms / 3'600'000, (total_ms / 60'000) % 60,
                  (total_ms / 1000) % 60, total_ms % 1000);
    return buf;
}

// Static: the geometry and engine are tens of KB (sized like the firmware's).
neotree::LedGeometry geometry;
neotree::Engine engine;

}  // namespace

int main(int argc, char **argv)
{
    double duration_s = 60.0;
    unsigned fps = 60;
    unsigned tick_hz = 120;
    unsigned seed = 1;
    unsigned render_every = 1;
    bool quiet = false;
    std::string positions = NEOTREE_SIM_DEFAULT_POSITIONS;
    std::string scene = "empty";

    for (int i = 1; i < argc; i++)
    {
        std::string a = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc)
            {
                std::fprintf(stderr, "%s needs a value\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--duration")
        {
            duration_s = neotree::sim::parse_duration_s(value());
        }
        else if (a == "--fps")
        {
            fps = static_cast<unsigned>(std::strtoul(value().c_str(), nullptr, 10));
        }
        else if (a == "--tick-hz")
        {
            tick_hz = static_cast<unsigned>(std::strtoul(value().c_str(), nullptr, 10));
        }
        else if (a == "--seed")
        {
            seed = static_cast<unsigned>(std::strtoul(value().c_str(), nullptr, 10));
        }
        else if (a == "--render-every")
        {
            render_every = static_cast<unsigned>(std::strtoul(value().c_str(), nullptr, 10));
        }
        else if (a == "--positions")
        {
            positions = value();
        }
        else if (a == "--scene")
        {
            scene = value();
        }
        else if (a == "--quiet")
        {
            quiet = true;
        }
        else
        {
            usage();
            return a == "--help" || a == "-h" ? 0 : 2;
        }
    }
    if (duration_s < 0.0 || fps == 0 || tick_hz == 0)
    {
        usage();
        return 2;
    }

    std::string error;
    if (!neotree::sim::load_positions_csv(positions, geometry, error))
    {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    neotree::EngineConfig config;
    config.tick_hz = tick_hz;
    config.seed = seed;
    config.max_ticks_per_advance = 0;   // never drop time: run every tick
    engine.init(geometry, config, quiet ? nullptr : log_to_stderr);
    if (!neotree::sim::setup_demo(engine, scene))
    {
        std::fprintf(stderr, "unknown scene '%s' (have: %s)\n", scene.c_str(),
                     neotree::sim::demo_scene_names().c_str());
        return 2;
    }

    std::vector<neotree::Rgb> frame(geometry.count());
    const int64_t duration_us = static_cast<int64_t>(duration_s * 1e6);
    const uint64_t total_frames = static_cast<uint64_t>(duration_us) * fps / 1'000'000;

    auto start = std::chrono::steady_clock::now();
    int64_t last_frame_time_us = 0;
    uint64_t rendered = 0;
    uint64_t led_evals = 0;
    for (uint64_t f = 1; f <= total_frames; f++)
    {
        // Frame times from integer math, so no drift across long runs.
        int64_t t_us = static_cast<int64_t>(f * 1'000'000 / fps);
        engine.advance(t_us - last_frame_time_us);
        last_frame_time_us = t_us;
        if (render_every != 0 && f % render_every == 0)
        {
            neotree::sim::update_demo(engine, scene);
            engine.render(frame);
            rendered++;
            led_evals += engine.stats().last_frame_led_evals;
        }
    }
    engine.advance(duration_us - last_frame_time_us);   // any remainder past the last frame
    double wall_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    const neotree::EngineStats &st = engine.stats();
    double sim_s = static_cast<double>(engine.time_us()) / 1e6;
    std::printf("sim time      %s (%.3f s)\n", format_time(sim_s).c_str(), sim_s);
    std::printf("wall time     %.3f s\n", wall_s);
    std::printf("speed         %.0fx real time\n", wall_s > 0.0 ? sim_s / wall_s : 0.0);
    std::printf("ticks         %llu at %u Hz (%.0f/s wall), dropped %llu\n", (unsigned long long)st.ticks, tick_hz,
                wall_s > 0.0 ? static_cast<double>(st.ticks) / wall_s : 0.0, (unsigned long long)st.ticks_dropped);
    std::printf("frames        %llu rendered of %llu at %u fps (%.0f/s wall)\n", (unsigned long long)rendered,
                (unsigned long long)total_frames, fps, wall_s > 0.0 ? static_cast<double>(rendered) / wall_s : 0.0);
    std::printf("entities      %u live, %u failed spawns\n", (unsigned)st.entities, (unsigned)st.spawns_failed);
    const neotree::BehaviorStats &bs = engine.behavior().stats();
    std::printf("behavior      peak %u entities, %u rule fires, %u events (%u dropped), %u actions over budget, "
                "%u spawns over quota, %u contacts dropped\n",
                (unsigned)bs.peak_entities, (unsigned)bs.rule_fires, (unsigned)bs.events, (unsigned)bs.events_dropped,
                (unsigned)bs.actions_dropped, (unsigned)bs.spawns_over_quota, (unsigned)bs.contacts_dropped);
    std::printf("scene         %s:%.0f LED-layer evaluations per frame, %.1f us per frame on this PC\n", scene.c_str(),
                rendered ? static_cast<double>(led_evals) / static_cast<double>(rendered) : 0.0,
                rendered ? wall_s * 1e6 / static_cast<double>(rendered) : 0.0);
    std::printf("LEDs          %u (%u positioned, %u mapped)\n", (unsigned)geometry.count(),
                (unsigned)geometry.positioned_count(), (unsigned)geometry.mapped_count());
    return 0;
}
