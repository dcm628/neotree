# Host simulator

The rendering engine (`engine/`) built for the PC, so modes can be developed
and tested without the tree or a complete map. Design: `docs/RENDERER.md` §11.

| Program | What it does |
|---|---|
| `neotree_sim` | Headless. Runs the engine back to back, far faster than real time, and reports speed and stats. |
| `neotree_view` | Interactive 3D view of the engine's output on the tree's LED positions. |
| `neotree_engine_tests` | Engine unit tests (run by `ctest`). |

## Setup (once)

A C++ compiler for Windows programs: WinLibs GCC, installed per-user with no
admin prompt:

```powershell
winget install --id BrechtSanders.WinLibs.POSIX.UCRT --exact --scope user
```

CMake and Ninja come from the Pico SDK install (or WinLibs). The first viewer
build downloads raylib; the first test build downloads doctest.

## Build and test

```powershell
tools\build_sim.ps1            # configure if needed, build, run tests
tools\build_sim.ps1 -Clean     # from scratch
tools\build_sim.ps1 -NoViewer  # skip raylib
```

Outputs land in `sim\build\`.

## Run

```powershell
sim\build\neotree_sim.exe --duration 8h                  # 8 hours of sim time
sim\build\neotree_sim.exe --duration 8h --render-every 0 # simulation only
sim\build\neotree_view.exe                               # interactive
sim\build\neotree_view.exe --colors source --screenshot out.png
```

`neotree_sim` options: `--duration T` (s/m/h/d suffixes), `--fps N`,
`--tick-hz N`, `--seed N`, `--render-every N` (0 = never), `--positions FILE`,
`--quiet`.

Viewer controls: left-drag orbit, right-drag raise/lower, wheel zoom; Space
pause, Right one tick while paused, Up/Down speed (0.1x–1000x), PgUp jump
+1 min (Shift +10 min), C cycle colors (engine output / position source /
height), R restart.

## LED positions

`data/tree_positions.csv` holds the 233 mapped positions from the firmware's
current map plus synthetic stand-ins for the other 767, drawn to match the
measured height and radius spread. Regenerate it after the map changes:

```powershell
python mapping\generate_sim_positions.py
```

## Engine code rules

The engine builds for both the Pico and the PC, so code in `engine/` uses no
exceptions, no RTTI, no heap allocation after init, no iostream, and float
(not double) in anything that runs per tick or per frame. The host build
enforces the first two, so violations fail here before they reach firmware.
