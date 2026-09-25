// neotree_view - interactive 3D view of the engine running on the tree's LED
// positions (docs/RENDERER.md section 11.1).
//
//   neotree_view [--positions FILE] [--seed N] [--tick-hz N]
//                [--scene NAME] [--colors output|source|height] [--at SECONDS]
//                [--screenshot FILE.png]
//
// --at starts the simulation that far in. --screenshot renders a moment,
// saves the window to FILE.png, and exits (for checking the view without
// watching it).
//
// Controls:
//   mouse      left-drag orbit, right-drag raise/lower, wheel zoom
//   Space      pause / resume          Right   one tick (while paused)
//   Up / Down  speed x2 / /2 (0.1x - 1000x)
//   PgUp       jump +1 min (Shift: +10 min) - simulated, not rendered
//   C          color view: engine output / position source / height
//   S          next demo scene
//   R          restart from t = 0 with the same seed

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "neotree/engine.hpp"
#include "demo_scenes.hpp"
#include "positions_csv.hpp"
#include "raylib.h"

namespace {

using neotree::Vec3;

// Engine space is z-up millimeters; raylib is y-up. (x, y, z) -> (x, z, -y)
// is a rotation, so handedness (and which way the tree turns) is preserved.
Vector3 to_view(Vec3 p) { return {p.x / 1000.0f, p.z / 1000.0f, -p.y / 1000.0f}; }

const float speeds[] = {0.1f, 0.25f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f, 25.0f, 50.0f, 100.0f, 250.0f, 500.0f, 1000.0f};
const int speed_count = sizeof(speeds) / sizeof(speeds[0]);
const int speed_default = 3;   // 1x

enum class ColorView
{
    output,
    source,
    height,
    count
};
const char *color_view_name(ColorView v)
{
    switch (v)
    {
    case ColorView::output: return "engine output";
    case ColorView::source: return "position source (orange mapped, blue synthetic)";
    case ColorView::height: return "height";
    default: return "";
    }
}

std::string format_time(int64_t us)
{
    long long ms = us / 1000;
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld.%03lld", ms / 3'600'000, (ms / 60'000) % 60,
                  (ms / 1000) % 60, ms % 1000);
    return buf;
}

unsigned char to_byte(float v) { return static_cast<unsigned char>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); }

Color height_color(float h)
{
    // Blue at the bottom through green to red at the top.
    Color c = ColorFromHSV(240.0f * (1.0f - h), 0.85f, 1.0f);
    return c;
}

neotree::LedGeometry geometry;
neotree::Engine engine;

}  // namespace

int main(int argc, char **argv)
{
    std::string positions = NEOTREE_SIM_DEFAULT_POSITIONS;
    std::string screenshot;
    std::string scene = "layers";
    double start_at_s = 0.0;
    ColorView view = ColorView::output;
    neotree::EngineConfig config;
    config.max_ticks_per_advance = 0;
    for (int i = 1; i + 1 < argc; i += 2)
    {
        std::string a = argv[i];
        if (a == "--positions")
        {
            positions = argv[i + 1];
        }
        else if (a == "--seed")
        {
            config.seed = static_cast<uint32_t>(std::strtoul(argv[i + 1], nullptr, 10));
        }
        else if (a == "--tick-hz")
        {
            config.tick_hz = static_cast<uint32_t>(std::strtoul(argv[i + 1], nullptr, 10));
        }
        else if (a == "--colors")
        {
            std::string v = argv[i + 1];
            view = v == "source" ? ColorView::source : v == "height" ? ColorView::height : ColorView::output;
        }
        else if (a == "--scene")
        {
            scene = argv[i + 1];
        }
        else if (a == "--screenshot")
        {
            screenshot = argv[i + 1];
        }
        else if (a == "--at")
        {
            start_at_s = std::strtod(argv[i + 1], nullptr);
        }
    }

    std::string error;
    if (!neotree::sim::load_positions_csv(positions, geometry, error))
    {
        std::fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    engine.init(geometry, config);
    if (!neotree::sim::setup_demo(engine, scene))
    {
        std::fprintf(stderr, "unknown scene '%s' (have: %s)\n", scene.c_str(),
                     neotree::sim::demo_scene_names().c_str());
        return 2;
    }
    const char *scene_cycle[] = {"layers", "wedge", "sweep_linear", "sweep_gravity", "sweep_launch",
                                 "bounce", "snow",  "orbit",        "canvas",        "empty"};
    const int scene_count = sizeof(scene_cycle) / sizeof(scene_cycle[0]);
    engine.advance(static_cast<int64_t>(start_at_s * 1e6));
    std::vector<neotree::Rgb> frame(geometry.count());

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
    InitWindow(1280, 860, "NeoTree viewer");
    SetTargetFPS(60);

    const neotree::Bounds &b = geometry.bounds();
    float target_y = (b.min.z + b.max.z) / 2000.0f;
    float yaw = 0.6f;
    float pitch = 0.25f;
    float distance = 4.5f;

    int speed_index = speed_default;
    bool paused = false;
    int frames_drawn = 0;

    while (!WindowShouldClose())
    {
        // ---- input ----
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT))
        {
            Vector2 d = GetMouseDelta();
            yaw -= d.x * 0.008f;
            pitch = std::clamp(pitch + d.y * 0.008f, -1.4f, 1.4f);
        }
        if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT))
        {
            target_y += GetMouseDelta().y * 0.004f;
        }
        distance = std::clamp(distance * (1.0f - GetMouseWheelMove() * 0.1f), 0.5f, 20.0f);

        if (IsKeyPressed(KEY_SPACE))
        {
            paused = !paused;
        }
        if (IsKeyPressed(KEY_UP))
        {
            speed_index = std::min(speed_index + 1, speed_count - 1);
        }
        if (IsKeyPressed(KEY_DOWN))
        {
            speed_index = std::max(speed_index - 1, 0);
        }
        if (IsKeyPressed(KEY_C))
        {
            view = static_cast<ColorView>((static_cast<int>(view) + 1) % static_cast<int>(ColorView::count));
        }
        if (IsKeyPressed(KEY_R))
        {
            engine.init(geometry, config);
            neotree::sim::setup_demo(engine, scene);
        }
        if (IsKeyPressed(KEY_S))
        {
            int next = 0;
            for (int k = 0; k < scene_count; k++)
            {
                if (scene == scene_cycle[k])
                {
                    next = (k + 1) % scene_count;
                }
            }
            scene = scene_cycle[next];
            neotree::sim::setup_demo(engine, scene);
        }
        if (IsKeyPressed(KEY_PAGE_UP))
        {
            bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
            engine.advance((shift ? 600LL : 60LL) * 1'000'000);
        }

        // ---- simulate ----
        if (paused)
        {
            if (IsKeyPressed(KEY_RIGHT))
            {
                engine.run_ticks(1);
            }
        }
        else
        {
            // Cap one frame's real time so a stall (window drag) doesn't jump.
            float real_dt = std::min(GetFrameTime(), 0.1f);
            engine.advance(static_cast<int64_t>(real_dt * speeds[speed_index] * 1e6f));
        }
        neotree::sim::update_demo(engine, scene);
        engine.render(frame);

        // ---- draw ----
        Camera3D cam{};
        cam.target = {0.0f, target_y, 0.0f};
        cam.position = {cam.target.x + distance * std::cos(pitch) * std::sin(yaw),
                        cam.target.y + distance * std::sin(pitch),
                        cam.target.z + distance * std::cos(pitch) * std::cos(yaw)};
        cam.up = {0.0f, 1.0f, 0.0f};
        cam.fovy = 45.0f;
        cam.projection = CAMERA_PERSPECTIVE;

        BeginDrawing();
        ClearBackground(Color{12, 12, 16, 255});
        BeginMode3D(cam);
        DrawGrid(12, 0.25f);
        DrawLine3D(to_view({0, 0, b.min.z}), to_view({0, 0, b.max.z}), Color{70, 55, 40, 255});
        for (uint16_t i = 0; i < geometry.count(); i++)
        {
            if (!geometry.has_position(i))
            {
                continue;
            }
            Vector3 p = to_view(geometry.position(i));
            Color c;
            float size = 0.012f;
            switch (view)
            {
            case ColorView::output:
            {
                // Linear engine output shown directly; the LED gamma curve
                // arrives with the master stage in M2.
                const neotree::Rgb &px = frame[i];
                if (px.r + px.g + px.b <= 0.0f)
                {
                    c = Color{45, 45, 50, 255};   // unlit, but keep the tree visible
                    size = 0.007f;
                }
                else
                {
                    c = Color{to_byte(px.r), to_byte(px.g), to_byte(px.b), 255};
                }
                break;
            }
            case ColorView::source:
                c = geometry.source(i) == neotree::PositionSource::mapped ? Color{255, 150, 40, 255}
                                                                          : Color{90, 120, 200, 255};
                break;
            default:
                c = height_color(geometry.height01(i));
                break;
            }
            DrawSphereEx(p, size, 4, 6, c);
        }
        EndMode3D();

        const neotree::EngineStats &st = engine.stats();
        DrawText(TextFormat("sim %s   speed %gx%s", format_time(engine.time_us()).c_str(), speeds[speed_index],
                            paused ? "   [paused]" : ""),
                 12, 10, 22, RAYWHITE);
        DrawText(TextFormat("ticks %llu (dropped %llu)   frames %llu   view %d fps", (unsigned long long)st.ticks,
                            (unsigned long long)st.ticks_dropped, (unsigned long long)st.frames, GetFPS()),
                 12, 38, 18, LIGHTGRAY);
        DrawText(TextFormat("scene: %s   %u entities   %u evals/frame", scene.c_str(),
                            (unsigned)engine.stats().entities, (unsigned)engine.stats().last_frame_led_evals),
                 12, 82, 18, LIGHTGRAY);
        DrawText(TextFormat("LEDs %u: %u mapped, %u synthetic   colors: %s", (unsigned)geometry.count(),
                            (unsigned)geometry.mapped_count(),
                            (unsigned)(geometry.positioned_count() - geometry.mapped_count()), color_view_name(view)),
                 12, 60, 18, LIGHTGRAY);
        DrawText("Space pause  Right step  Up/Down speed  PgUp +1 min (Shift +10)  C colors  S scene  R restart  "
                 "mouse: L orbit, R raise, wheel zoom",
                 12, GetScreenHeight() - 26, 16, GRAY);
        EndDrawing();

        if (!screenshot.empty() && ++frames_drawn == 10)
        {
            Image shot = LoadImageFromScreen();
            bool ok = ExportImage(shot, screenshot.c_str());
            UnloadImage(shot);
            std::fprintf(stderr, "%s %s\n", ok ? "saved" : "failed to save", screenshot.c_str());
            break;
        }
    }
    CloseWindow();
    return 0;
}
