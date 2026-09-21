# NeoPixel Volumetric Christmas Tree — Architecture

**Status:** First draft · **Owner:** Dan · **Last updated:** 2026-09-20

**Related:** [`DEVELOPMENT.md`](./DEVELOPMENT.md) — how the project is developed,
versioned, and deployed (dev environment, git, remote build/flash workflow).
This document is *what the system is*; that one is *how you work on it*.

> This document is the reference architecture for the project. It describes what
> the system is, how its pieces fit together, and where the boundaries and
> interfaces between them sit. It is intentionally high-level on the two
> unsolved problems (3D LED mapping and volumetric rendering) — those get their
> own design docs once the shape here is agreed. Open questions are collected at
> the end and flagged inline as **[OPEN]**.

---

## 1. Objective

Turn a physical Christmas tree strung with ~1000 addressable LEDs into a
**volumetric display**: a 3D point-cloud "screen" that can render patterns,
animations, and eventually simple dynamic simulations (fire, fluid, particle
effects, falling snow, etc.) as true 3D phenomena rather than per-string 1D
effects.

The defining idea is that every LED has a known position in 3D space. Once the
firmware knows *where each LED is*, an animation stops being "set pixel N on
string S" and becomes "evaluate the scene at coordinate (x, y, z) and light the
LED there accordingly." This is what separates this project from an ordinary
addressable-LED tree.

Three things must be true for that to work, and they map onto the three
subsystems below:

1. **Render** — the tree can be driven quickly and reliably as one logical set
   of 1000 pixels (largely exists today).
2. **Map** — the 3D coordinate of every LED is known and lives on the device
   that renders (partially exists; the hard, unfinished half).
3. **Control** — animations can be selected, parameterized, and triggered
   remotely over WiFi (not yet started).

---

## 2. System Overview

```
                    ┌────────────────────────────────────────────┐
                    │              CONTROL PLANE                   │
                    │   (phone / laptop / browser on the LAN)      │
                    └───────────────────────┬────────────────────┘
                                             │  WiFi / HTTP (or WS)
                                             ▼
   ┌─────────────────────────┐      ┌──────────────────────────────┐
   │   MAPPING RIG (offline)  │      │   RENDER TARGET (runtime)     │
   │                          │      │                               │
   │   RPi 4                  │      │   RPi Pico 2 W (RP2350)       │
   │   4× USB webcams         │      │   C++ Pico SDK firmware       │
   │   Python capture +       │      │   PIO + DMA → 4 LED strings   │
   │   centroid + 3D solve    │──────▶   Volumetric renderer         │
   │                          │ coord│   HTTP/WS control endpoint    │
   │   Outputs coordinate map │ config│                              │
   └─────────────────────────┘      └───────────────┬──────────────┘
                                                     │  WS2812 (800 kHz)
                                                     ▼
                                     ┌───────────────────────────────┐
                                     │  4 × LED strings (~250 each)  │
                                     │  1000 LEDs @ known (x,y,z)    │
                                     └───────────────────────────────┘
```

The mapping rig runs **offline / at setup time**. Its only job is to produce a
coordinate map, which is handed to the render target once and then baked in. At
runtime the Pico is the whole show: it holds the coordinate map, runs the
renderer, and exposes the control interface. The Pi is not in the runtime path.

---

## 3. Hardware

### 3.1 Render target — Raspberry Pi Pico 2 W

> **Hardware status (2026-09-20):** the tree currently runs a **Pico W
> (RP2040)** — that's what the existing firmware builds and flashes for today.
> The project is migrating to the **Pico 2 W (RP2350)**, which is the platform
> this architecture targets (the RP2350's FPU and larger SRAM are what make the
> volumetric rendering goals practical). The RP2350 details below describe that
> target; migration specifics are deferred.

- **MCU:** RP2350, dual-core (Arm Cortex-M33 @ 150 MHz default; RISC-V Hazard3
  cores also selectable). The second core is available and is a natural place to
  split "render the next frame" from "drive the current frame out / service the
  network."
- **Memory:** 520 KB on-chip SRAM, ~4 MB onboard QSPI flash. Comfortable for
  this workload — see the memory budget in §4.3.
- **Wireless:** CYW43439, 2.4 GHz WiFi + BLE. WiFi is the control transport.
- **Why it fits:** the RP2350's **PIO** blocks are the key feature. PIO state
  machines generate the tight WS2812 bit-timing in hardware, so the CPU never
  bit-bangs the protocol and is free to render. RP2350 has 3 PIO blocks × 4
  state machines = 12 SMs, so all 4 strings can be driven in parallel with room
  to spare.

### 3.2 LEDs

- **Count:** ~1000, in **4 strings of ~250** each. **[OPEN]** confirm exact
  per-string counts — they may be uneven and the map/renderer must not assume
  equal lengths.
- **Type:** WS2812-family ("NeoPixel"), single-wire, 24-bit GRB, ~800 kHz.
  **[OPEN]** confirm exact part (WS2812B vs SK6812/RGBW vs WS2815 12 V) — RGBW
  and 12 V variants change the framebuffer layout and power math.
- **Data:** one GPIO per string → level-shift to 5 V logic if needed → string.
  **[OPEN]** document the actual GPIO pin assignment per string.

### 3.3 Power

This is a real design constraint, not an afterthought. 1000 WS2812s at full
white draw ~60 mA each → **~60 A at 5 V ≈ 300 W** worst case. The system must:

- Enforce a **global brightness / power cap** in firmware (scale all output, or
  estimate per-frame current draw and clamp) so a full-white frame can't
  brown-out the supply.
- Inject power at multiple points along the strings to avoid voltage droop
  (documented as a wiring concern, not a firmware one).

**[OPEN]** capture the actual PSU rating(s) and injection points so the firmware
power cap can be set to something real.

### 3.4 Mapping rig — Raspberry Pi 4 + 4 webcams

- **RPi 4** running the capture + solve pipeline in Python.
- **4 USB webcams** arranged as **2 pairs of 2**. The pairing suggests a stereo
  strategy (two stereo baselines viewing the tree from different angles), which
  gives depth per pair and coverage of LEDs occluded from any single viewpoint.
- Used only at setup time; not powered/connected during normal operation.

---

## 4. Subsystem A — Render (Pico firmware)

**State: largely exists, drives LEDs but not yet volumetrically.**

### 4.1 Responsibilities

Own the LEDs as one logical 1000-pixel framebuffer; produce frames at a steady
rate; drive all 4 strings out via PIO + DMA; hold the coordinate map; run the
active animation; apply global brightness/power limiting; service the control
interface.

### 4.2 Pipeline (target)

```
 animation/scene  ──►  framebuffer   ──►  brightness/power  ──►  per-string
 evaluates color        (logical           + gamma/color         DMA buffers
 per LED using          1000-pixel         correction            (GRB, timed)
 (x,y,z) + time)        RGB array)                                    │
                                                                      ▼
                                                            4× PIO state machines
                                                                 → WS2812
```

The change from today's version is at the front of the pipeline: instead of
addressing pixels by strip index, the scene is a function evaluated **per LED at
its 3D coordinate** (see §6). Everything downstream — framebuffer → correction →
DMA → PIO — is unchanged and is where the existing, working code lives.

### 4.3 Memory budget (sanity check)

- Framebuffer (8-bit RGB): 1000 × 3 = **3 KB**. A higher-precision working
  buffer (e.g. 16-bit/channel for simulations) is still only ~6 KB.
- Coordinate map: 1000 × 3 floats = **12 KB** (or less if quantized to int16).
- DMA/PIO output buffers: a few KB.

Everything fits in SRAM with enormous headroom against 520 KB. Memory is not a
constraint; **per-frame compute time** is the thing to watch for simulations.

### 4.4 Concurrency

Natural split across the two cores: **Core 0** = networking/control + scene
state; **Core 1** = render loop (evaluate frame, hand buffers to DMA). DMA + PIO
do the actual output asynchronously, so the render loop's budget is roughly the
frame interval minus latch time. **[OPEN]** decide the core split and the
render/output double-buffering scheme.

### 4.5 Frame rate

WS2812 at 800 kHz = 30 µs/pixel. 250 pixels/string ≈ 7.5 ms, but the 4 strings
clock out in **parallel**, so the output floor is ~7.5 ms + reset ≈ **>100 fps
achievable**. Real frame rate will be gated by render compute, not output.
Target a fixed cadence (e.g. 30–60 fps) for predictable animation timing.

---

## 5. Subsystem B — Map (3D LED localization)

**State: capture + centroid works; 3D solve and coordinate export do NOT exist yet.**

### 5.1 Goal

Produce, for every LED index, a 3D coordinate `(x, y, z)` in a consistent tree
coordinate frame, plus a confidence/validity flag. This map is the bridge
between the physical tree and the renderer.

### 5.2 Current pipeline (working)

```
 for each LED n:
     turn all LEDs off, LED n → full white
     capture frame on all 4 cameras
     find center-of-mass of illumination in each image  ──►  (u,v) per camera
```

So today the output is, per LED, up to four 2D image-space centroids.

### 5.3 What's missing (high-level)

1. **Camera calibration** — intrinsics per camera, and extrinsics (relative
   poses) so the four views share one coordinate frame. Without this the 2D
   points can't be turned into 3D.
2. **2D → 3D reconstruction** — triangulate each LED's 3D position from the
   camera views that saw it (multi-view / stereo). Handle LEDs seen by only one
   camera or none.
3. **Robustness / cleanup** — reject bad detections (occlusion, reflection,
   bleed from a neighbor), fill or flag gaps, and place the result in a sensible
   tree frame (origin, up-axis, scale).
4. **Export** — serialize the finished map into the format the firmware ingests
   (§7.2) and get it onto the Pico (§7.3).

The detailed algorithm choices for calibration and reconstruction are a separate
design doc; this draft only fixes that these four stages exist and where the
subsystem boundary is. **[OPEN]** calibration method, reconstruction approach,
and how uneven coverage / occluded LEDs are handled.

### 5.4 Design considerations to carry forward

- The centroid-per-image approach is inherently 2D; **depth comes only from
  multiple views**, hence the stereo pairing. Coverage (every LED seen by ≥2
  cameras) is the thing that determines whether a full map is even possible from
  the current rig placement.
- The map is captured **once per physical setup**. It only needs redoing if the
  tree is re-strung or cameras move — so the pipeline can be slow and offline;
  correctness matters far more than speed.
- **[OPEN]** how is LED index ↔ physical LED correspondence guaranteed across
  the whole run (i.e. the firmware's ordering during mapping must match the
  runtime ordering exactly).

---

## 6. Volumetric Rendering (the core new capability)

**State: not started; this is the conceptual heart of the project.**

Once the renderer has `(x, y, z)` per LED, a "scene" is any function that assigns
a color to a point in space (and time):

```
    color = scene(x, y, z, t, params)
```

Each frame, the renderer evaluates this for all 1000 LED coordinates and writes
the results into the framebuffer. This single indirection is what makes the tree
volumetric — the same scene function produces correct 3D results regardless of
how the LEDs are physically arranged.

At a high level, scenes fall into a few families (detail deferred to a rendering
design doc):

- **Field/pattern scenes** — closed-form functions of position and time (planes
  sweeping through the volume, radial pulses, 3D noise, gradients, text/shapes
  swept through space). Cheap; pure function of `(x,y,z,t)`.
- **Particle scenes** — maintain particles with position/velocity in the tree's
  coordinate space; each frame, light LEDs near each particle (falling snow,
  rising embers, comets).
- **Simulation scenes** — evolve a state over time (simple fluid/fire/reaction
  effects). These are the compute-bound case and the reason to keep an eye on
  per-frame budget and possibly a coarse voxel grid the LEDs sample from.

Key design decisions for the rendering design doc: **[OPEN]** work in
normalized tree coordinates vs. metric; **[OPEN]** evaluate scenes directly at
LED points vs. maintain a voxel grid and interpolate LED colors from it (the
latter decouples sim resolution from LED count and is likely better for
simulations); **[OPEN]** fixed-point vs. float math on the M33.

---

## 7. Interfaces & Data Contracts

The subsystems are only as clean as the interfaces between them. These are the
seams to nail down early.

### 7.1 Mapping capture ↔ tree (during scan)

The Pi must command "all off, LED n on, full white" and know the capture is
settled before snapping. Today this exists ad hoc. **[OPEN]** define this
control path for the scan (is it the same control interface as §7.4, or a
dedicated scan mode/command?).

### 7.2 Coordinate map format (Pi → firmware)

The single most important data contract in the project. A per-LED record of at
least: `index`, `x`, `y`, `z`, `valid`. Proposed: emit a human-readable master
(CSV/JSON) from the Pi for inspection, and a compact binary form for the
firmware. **[OPEN]** exact schema, units, coordinate frame convention, and how
invalid/unmapped LEDs are represented so the renderer can skip or interpolate
them.

### 7.3 Getting the map onto the Pico

**[OPEN]** — undecided and worth deciding early:

- **(a)** Compile the map into the firmware as a generated C++ header/array and
  reflash. Simplest; requires a rebuild per remap.
- **(b)** Store the map in a reserved flash region and upload it separately (over
  USB, or over the network via the control interface). No firmware rebuild to
  remap; needs a small flash storage scheme.

Given the map changes rarely, (a) is the fastest path to first light;
(b) is the nicer long-term ergonomics. Recommend starting with (a).

### 7.4 Control interface (client → firmware)

Over WiFi. A small HTTP (and possibly WebSocket) API on the Pico exposing:
select active scene, set parameters (speed, colors, brightness), global on/off,
and a live/preview channel if desired. **[OPEN]** HTTP-only vs. HTTP + WebSocket
(WS is better for live sliders / continuous control); endpoint/message schema;
whether any state persists across reboot.

---

## 8. Subsystem C — Control (remote interface)

**State: not started.**

### 8.1 Options

- **Server on the Pico (recommended default):** lwIP is available through the
  Pico SDK; run a lightweight HTTP/WS server directly on the device. Keeps the
  runtime a single self-contained box (Pi not needed at runtime), which matches
  the architecture in §2. Constraint: keep the server light so it doesn't steal
  the render loop's time (hence the Core 0 / Core 1 split in §4.4).
- **Server on a companion (Pi or always-on host):** richer UI, easier to build,
  but reintroduces a second always-on device into the runtime path. Reasonable
  as a *fallback* or for a fancier UI later, talking to the Pico over a simple
  protocol.

Recommendation: **server on the Pico**, with a minimal static web UI served from
flash, so the whole runtime system is just the tree + Pico.

### 8.2 UI

A single static page (HTML/JS) served by the firmware: scene picker + parameter
controls, talking to the API in §7.4. **[OPEN]** scope of v1 UI.

---

## 9. Toolchains & Environments

| Subsystem | Language / SDK | Build / Run | Host |
|---|---|---|---|
| Render firmware | C++, Raspberry Pi Pico SDK (RP2350), PIO | VS Code + **Raspberry Pi Pico extension** (CMake, arm-none-eabi toolchain), flash via UF2/USB or SWD | Windows |
| Mapping | Python (OpenCV expected for calibration/triangulation, NumPy) | run on the Pi | RPi 4 |
| Control UI | HTML/CSS/JS (static), optionally a small build step | served from Pico flash | browser on LAN |
| Coordinate map tooling | Python (part of mapping) → C++ header or binary blob | Pi, then fed to firmware build/flash | RPi 4 → Pico |

**[OPEN]** confirm the mapping stack uses OpenCV (calibration + triangulation
are effectively free there) and pin versions.

> The concrete development environment, git strategy, and remote build/deploy
> pipeline that use these toolchains are specified in
> [`DEVELOPMENT.md`](./DEVELOPMENT.md). In short: firmware is built on the
> Windows desktop and flashed to the Pico remotely by the Pi 4 over the native
> USB link (picotool); Python runs on the Pi via VSCode Remote-SSH; everything
> is versioned in a private GitHub monorepo.

---

## 10. Repository / Module Layout (proposed)

A single repo with clear subsystem boundaries, or three repos — **[OPEN]**.
Proposed monorepo shape:

```
/firmware        # C++ Pico SDK project: PIO drivers, framebuffer,
                 #   renderer/scenes, control server, generated coord map
/mapping         # Python: capture, centroid, calibration, 3D solve, export
/coord-map       # generated artifacts: master CSV/JSON + firmware binary/header
/web-ui          # static control UI served from firmware flash
/docs            # this document + per-subsystem design docs
```

---

## 11. Current State vs. Target (honest snapshot)

| Capability | Today | Target |
|---|---|---|
| Drive 1000 LEDs on 4 strings | ✅ functional | keep; refactor front-end to be coordinate-driven |
| Volumetric rendering | ❌ not volumetric | scene(x,y,z,t) pipeline (§6) |
| Global brightness / power cap | ❓ unconfirmed | firmware-enforced (§3.3) |
| Mapping: single-LED capture + centroid | ✅ works (Pi, 4 cams) | keep as capture stage |
| Camera calibration | ❌ | intrinsics + extrinsics (§5.3) |
| 2D → 3D reconstruction | ❌ | multi-view triangulation (§5.3) |
| Coordinate map export format | ❌ | defined contract (§7.2) |
| Map onto Pico | ❌ | header or flash upload (§7.3) |
| WiFi control interface | ❌ not started | HTTP/WS server on Pico (§8) |
| Control UI | ❌ | static web UI (§8.2) |

---

## 12. Suggested Sequencing

A dependency-ordered path (not a schedule):

1. **Lock the coordinate map contract (§7.2)** — everything downstream depends
   on it, so define it before writing the solver or the volumetric renderer.
2. **Finish mapping (§5.3):** calibration → triangulation → export a real map
   for the current tree.
3. **Get the map onto the Pico (§7.3)** — start with the compiled-header path.
4. **Volumetric renderer v1 (§6):** refactor the render front-end to be
   coordinate-driven; ship a couple of field/pattern scenes to validate the map
   visually (a sweeping plane instantly shows whether the map is correct).
5. **Control interface (§8):** HTTP server + minimal UI; wire scene selection
   and parameters.
6. **Simulations (§6):** particle and sim scenes once the pipeline and budget
   are proven.

Step 4 doubles as the **map validation tool** — a plane or gradient swept
through the volume makes any mapping error obvious to the eye.

---

## 13. Open Questions (collected)

- Exact per-string LED counts (even vs. uneven).
- Exact LED part (WS2812B / SK6812 RGBW / WS2815 12 V) — affects framebuffer &
  power.
- GPIO pin assignment per string; level shifting.
- PSU rating(s) and power-injection points → firmware power-cap value.
- Camera calibration method and multi-view reconstruction approach.
- Handling of occluded / low-coverage / unmapped LEDs.
- Guaranteeing LED index ↔ physical LED correspondence between scan and runtime.
- Coordinate map schema, units, and coordinate-frame convention.
- Map delivery to Pico: compiled header (a) vs. flash upload (b).
- Control transport: HTTP-only vs. HTTP + WebSocket; persistence across reboot.
- Rendering: normalized vs. metric coords; direct point evaluation vs. voxel
  grid + interpolation; fixed-point vs. float.
- Core 0 / Core 1 split and double-buffering scheme.
- Repo layout: monorepo vs. multi-repo.
- Windows flashing: USB/UF2 vs. SWD (+ debugging).
- Mapping stack: confirm OpenCV + versions.

---

*Next step: turn §5 (mapping) and §6 (volumetric rendering) into their own
detailed design docs once the coordinate map contract in §7.2 is settled.*
