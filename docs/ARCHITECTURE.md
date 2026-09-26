# NeoPixel Volumetric Christmas Tree — Architecture

**Status:** First draft · **Owner:** Dan · **Last updated:** 2026-09-23

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
                    │   native Android app (Kotlin) on family      │
                    │   phones/tablets, home WiFi LAN only         │
                    └───────────────────────┬────────────────────┘
                                             │  WiFi: custom protocol
                                             │  (commands + live stream)
                                             ▼
   ┌─────────────────────────┐      ┌──────────────────────────────┐
   │   MAPPING RIG (offline)  │      │   RENDER TARGET (runtime)     │
   │                          │      │                               │
   │   RPi 4                  │      │   RPi Pico 2 W (RP2350)       │
   │   4× USB webcams         │      │   C++ Pico SDK firmware       │
   │   Python capture +       │      │   PIO + DMA → 4 LED strings   │
   │   centroid + 3D solve    │──────▶   Volumetric renderer         │
   │                          │ coord│   Control protocol server     │
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

> **Hardware status (2026-09-23):** the tree still physically runs a **Pico W
> (RP2040)**, but the move to the **Pico 2 W (RP2350)** is decided and
> permanent — the RP2350's FPU and larger SRAM are what make the volumetric
> rendering and multi-client networking goals practical. A bare Pico 2 W
> testbed already runs the current firmware and is on WiFi (see
> DEVELOPMENT.md); the RP2040 build is kept only until the tree's board is
> swapped.

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

Natural split across the two cores: one core for networking/control, the other
for the render loop (evaluate frame, hand buffers to DMA). DMA + PIO do the
actual output asynchronously, so the render loop's budget is roughly the frame
interval minus latch time.

**As built (2026-09-23)** the split is the other way round: **core 0** renders
and applies commands; **core 1** reads USB serial and owns the CYW43/lwIP
stack (its IRQs run there). That's deliberate for now — core 0's current output
path masks interrupts for ~9 ms per string, which networking must not share.
Commands cross from core 1 to core 0 through a single-slot handoff today; the
control interface needs a real queue (§7.4). **[OPEN]** revisit the split once
output moves to DMA, and the render/output double-buffering scheme.

### 4.5 Frame rate

WS2812 at 800 kHz = 30 µs/pixel. 250 pixels/string ≈ 7.5 ms, but the 4 strings
clock out in **parallel**, so the output floor is ~7.5 ms + reset ≈ **>100 fps
achievable**. Real frame rate will be gated by render compute, not output.
Target a fixed cadence (e.g. 30–60 fps) for predictable animation timing.

**As built (2026-09-24):** output runs on DMA with the CPU free. Strings go
out in two phases, {1,2} then {3,4}, taking 15 ms per frame (~65 fps max).
The target is 60 fps. All four in parallel (9 ms) glitched on the real tree.
See [`LED_OUTPUT.md`](./LED_OUTPUT.md) for measurements, what was ruled out,
and how to revisit.

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

Calibration's plan, status and notes are in [CALIBRATION.md](CALIBRATION.md)
(waiting on a replacement board, 2026-09-25). The detailed algorithm choices
for reconstruction are a separate design doc; this draft only fixes that these four stages exist and where the
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

**Update 2026-09-24:** the rendering design doc is [`RENDERER.md`](./RENDERER.md)
(layered compositor + entity simulation + stackable modes). It answers the three
questions above: metric mm in float, direct evaluation at LED points (voxel
grid later as a layer type), float math.

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

Over WiFi, between the Android app (§8) and a protocol server on the Pico.
Because the only client is our own native app, this is a **custom protocol**,
not HTTP — no web server or web assets on the Pico. Decided (2026-09-23):

- **Two channels.**
  - A **reliable command channel** carries discrete commands and state:
    select mode, set a parameter, power, brightness, and query state/schema.
    Every connected client receives state changes, so several family phones
    stay in sync.
  - A **low-latency stream channel** carries continuous input: live slider
    drags, and later the phone-sensor paintbrush at ~30–60 updates/s. On this
    channel a late update is dropped, never queued behind newer ones.
  - Expected mapping: TCP for commands, UDP for the stream. Measured ping on
    the home network is ~10 ms average with WiFi power-save off.
- **Self-describing modes.**
  - Each animation mode declares its parameters: name, type (number range,
    color, toggle, choice), default and label.
  - The app builds its controls from that schema.
  - Modes don't exist yet (they move from the Pi's Python scripts into
    firmware), so the interface is generic: a new mode or option appears in
    the app with no app changes.
- **Manual color control is a first-class mode:** whole tree, individual LEDs
  and groups, and 3D regions, mirroring what the USB serial protocol already
  exposes. The goal-3 paintbrush is manual mode driven by phone orientation.
- **One command path.** Network and USB serial commands feed the same
  thread-safe queue into the same dispatcher. The USB serial protocol stays
  for mapping and deploy; it's limited to one ≤64-byte message per USB packet.
- **Persistence.** The active mode, its parameters and the brightness survive a
  power cycle. They're stored in flash, like the WiFi credentials.
- **Discovery.** The Pico advertises itself with mDNS/DNS-SD, and the app finds
  it with Android's built-in network service discovery. A DHCP reservation in
  the router is the fallback. No IP typing.
- **Security.** LAN only, on the secured home WiFi; no port forwarding, no app
  auth.

**Phase A command channel (proposed 2026-09-23):** the existing binary serial
message types, each with a length prefix, over TCP. **[OPEN]** the encoding of
the mode schema when modes arrive (phase B), protocol versioning, max
concurrent clients, and exactly which state persists.

---

## 8. Subsystem C — Control (remote interface)

**State (2026-09-23): phase A working on the Pico 2 W testbed.**

- The Pico is on WiFi with auto-reconnect and USB-provisioned credentials.
- **Command server:** TCP port 7777, up to 4 clients, the existing binary
  color commands length-prefixed and ACKed per message
  (`firmware/include/neo_tree_net_server.hpp`). It shares the command queue
  with USB serial.
- **Discovery:** mDNS answers for `neotree.local` and advertises
  `_neotree._tcp`.
- **Clients:**
  - The Android app v1 (`android/`) finds the tree, and offers fill,
    background, off, region painting and single LEDs, with live sliders.
  - `mapping/neotree_net.py` lets the existing Python helpers drive the tree
    over WiFi.
- **Not yet done:** modes, persistence and the stream channel (phase B).
  LED output is untested visually, since the testbed has no LEDs.

### 8.0 The clock (2026-09-25)

The tree keeps wall-clock time, for timers (what they do is still to be
decided). `firmware/src/neo_tree_clock.cpp`:

- **UTC from SNTP** (lwIP's client) - `pool.ntp.org`, then `time.google.com`.
  The router doesn't answer NTP. It syncs once the WiFi link is up, then
  hourly, with the round trip compensated. Before the first sync its clock
  starts at a floor date (2026-05-28), because lwIP skips the compensation
  when the clock is more than ~34 years out.
- **The app's time is a fallback**: TIME_SET (47) is taken only if SNTP
  hasn't synced for 2 hours. The app sends it on every connect.
- **Local time from a POSIX TZ rule** (e.g. `PST8PDT,M3.2.0,M11.1.0`).
  - NTP carries no time zone, so the tree has a default: Los Angeles, its
    home (`clock_default_tz`). Local time is right from power-up, with no
    phone needed.
  - Another zone can be set on purpose: the app's Debug page offers the
    phone's zone (TIME_ZONE, 48) when it differs; an empty TIME_ZONE goes back to
    the default. It's not sent
    automatically, so a phone back from a trip can't move the tree.
  - A set zone is stored in a one-sector settings region below the effects
    (`key=value` text), and written only when it changes.
  - The engine converts with it, DST changes included, with no zone
    database (`engine/include/neotree/civil_time.hpp`).
- Kept as an offset from the microsecond timer, so a power cut loses it
  until the next sync. After a reboot it was set 13 s after power-up.
- Status JSON `clock`: set, source, `unix_ms`, `local`, tz, `offset_min`,
  dst, sync ages, counts, and the last correction. The app's Debug page has a
  Clock card.
- Measured: within ~10-15 ms of true time (the measurement itself is good to
  about +-12 ms),
  and drifting ~2 ppm between syncs (2 ms in 15 minutes; ~0.2 s a day with no
  sync at all).

The schedule built on it (lights on/off by day, events at set moments) is
in RENDERER.md §15, "The schedule".

### 8.1 Goals

The whole family uses it on the living-room tree at Christmas, from Android
phones and tablets on the home WiFi.

1. **Basic control:** power, brightness, manual colors, pick an animation mode
   and its options.
2. **Interactive GUIs:** live sliders and color pickers, and a 3D view of the
   tree (built from the LED map) with touch painting on the model.
3. **Phone-sensor paintbrush (later):** point the phone at the tree and paint
   with it, driven by the phone's orientation. This needs an accurate 3D map
   (§5) and a "point at the tree centre and tap" calibration.

Goals 1–2 come first; goal 3 shapes the design (the stream channel,
manual-mode primitives, the native app).

### 8.2 Decisions (2026-09-23)

- **The server runs on the Pico; no companion device at runtime.** Plug the
  tree in and it works, and the Pi mapping rig stays out of the living room.
- **The client is a native Android app in Kotlin + Jetpack Compose.** Every
  household device is Android.
  - A browser can't read motion sensors over plain HTTP; Chrome restricts
    them to secure contexts, and HTTPS with certificates on the Pico is
    awkward.
  - A native app also gets UDP, OS service discovery, haptics and widgets,
    and leaves the Pico simpler (no web server, no web assets in flash).
  - Costs accepted: Android only, and the app must be installed on each
    device.
- **Distribution (own devices only, no public Play listing):**
  - `adb install`, over USB or wireless debugging, for development.
  - For family devices, either a side-loaded APK from a download link or a
    Play Console internal-testing track. The internal track costs $25 once
    and gives Play-managed installs and auto-updates.
  - Keep the signing keystore safe: updates only install over an app signed
    with the same key.
  - Watch Google's 2026–27 developer-verification rollout for side-loaded
    apps. As announced, adb installs aren't affected.

### 8.3 UI

**[OPEN]** scope of the v1 screens. Minimum for goal 1: connection/discovery,
power and brightness, manual color, and a mode picker with controls generated
from the mode's parameter schema (§7.4).

---

## 9. Toolchains & Environments

| Subsystem | Language / SDK | Build / Run | Host |
|---|---|---|---|
| Render firmware | C++, Raspberry Pi Pico SDK (RP2350), PIO | VS Code + **Raspberry Pi Pico extension** (CMake, arm-none-eabi toolchain), flash via UF2/USB or SWD | Windows |
| Mapping | Python (OpenCV expected for calibration/triangulation, NumPy) | run on the Pi | RPi 4 |
| Control app | Kotlin + Jetpack Compose (Android) | Android Studio / Gradle on the desktop, installed via adb or APK | Windows → Android devices |
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
/android         # Kotlin control app (Gradle project)
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
| WiFi link | ✅ station mode, auto-reconnect, USB-provisioned creds (Pico 2 W testbed) | keep |
| Animation modes in firmware | ❌ live in Pi Python scripts | self-describing modes on the Pico (§7.4) |
| Control protocol | ✅ command channel (TCP 7777) + mDNS discovery; stream channel not yet | command + stream channels on the Pico (§7.4) |
| Control app | ✅ v1: discovery + color control (android/) | native Android app (§8) |

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
5. **Control interface (§7.4, §8):** protocol server on the Pico plus the
   Android app. Wire up mode selection and parameters.
6. **Simulations (§6):** particle and sim scenes once the pipeline and budget
   are proven.

Step 4 doubles as the **map validation tool** — a plane or gradient swept
through the volume makes any mapping error obvious to the eye.

**Actual order (2026-09-23):** mapping (step 2) is paused until a ChArUco
calibration board arrives. Control work therefore goes ahead first, in three
phases:

- **Phase A (goal 1), descoped to color control only (2026-09-23).**
  - Firmware: one command queue shared by serial and network; a TCP command
    server; mDNS discovery.
  - The server carries the **existing binary color commands** (single LED,
    group, whole-tree overlay/base color, cartesian/cylindrical volume), each
    with a length prefix. It uses the same message types and dispatcher as
    USB serial, without the 64-byte-per-message limit.
  - A basic Android app to find the tree and set colors.
  - Moved out of phase A: the self-describing mode framework, porting the
    Pi's animations, and persisted state.
- **Phase B (goal 2).** The mode framework and animation ports, the live
  stream channel, multi-client sync, and the 3D tree view with touch painting.
- **Phase C (goal 3).** The sensor paintbrush.

Only the 3D-view and paintbrush parts depend on an accurate map.

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
- Control protocol: message encoding, schema format, versioning, max clients,
  and which state persists (§7.4). Transport and client are decided — see §8.2.
- v1 app screen scope (§8.3).
- Rendering: normalized vs. metric coords; direct point evaluation vs. voxel
  grid + interpolation; fixed-point vs. float.
- Core 0 / Core 1 split and double-buffering scheme.
- Repo layout: monorepo vs. multi-repo.
- Windows flashing: USB/UF2 vs. SWD (+ debugging).
- Mapping stack: confirm OpenCV + versions.

---

*Next step: turn §5 (mapping) and §6 (volumetric rendering) into their own
detailed design docs once the coordinate map contract in §7.2 is settled.*
