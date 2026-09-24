# NeoTree Rendering Engine — Design

**Status:** Design agreed · M1 (engine skeleton + simulator) done 2026-09-24 · **Owner:** Dan · **Last updated:** 2026-09-24

**Related:** [`ARCHITECTURE.md`](./ARCHITECTURE.md) §6 defers volumetric
rendering to this document. [`LED_OUTPUT.md`](./LED_OUTPUT.md) covers the
DMA output stage this engine feeds.

> This is the design for the firmware's rendering engine: a layered compositor
> plus an entity (particle/object) simulation, with preprogrammed "modes" built
> on top. Decisions already agreed are marked **[DECIDED]**; proposals awaiting
> review are unmarked; open questions are **[OPEN]** and collected in §13.

---

## 1. Goals

1. A **true rendering engine** on the Pico: effects are built from reusable
   parts (layers, entities, forces, rules) instead of each animation being
   hand-written code.
2. **Layers**: flat-color (static) layers and other layer types, stacked and
   blended.
3. **Entities** that move, respond to external parameters (gravity, wind, a
   phone's tilt), and interact with each other — collide or pass through.
4. **Modes** (the discrete preprogrammed effects the family picks from the
   app) are built *on top of* the engine, can be **stacked** (e.g. snow over a
   rainbow), and can **loop, chain, or revert** when they end.
5. Designed so **direct entity control from a phone** and the **phone-sensor
   paintbrush** slot in later without reworking the core.
6. A **host simulator** runs the same engine on the PC, in real time or much
   faster than real time.

Non-goals for the first revision: direct phone control of entities (designed
for, not built), user-authored modes from the app, fluid/fire simulation.

## 2. Constraints

- **Hardware:** RP2350, 2× Cortex-M33 @ 150 MHz with single-precision FPU,
  520 KB SRAM. Core1 runs WiFi/USB; **core0 owns the engine** (it is ~99% idle
  at 60 fps since the DMA output rework).
- **Frame budget:** 60 fps target = 16.7 ms ≈ 2.5 M cycles per frame for
  simulation + render + frame prep. Overclocking to 200 MHz is available
  headroom, not the plan.
- **LEDs:** 1000 LEDs on 4 strings. They fill the tree's **volume**, not just
  its surface: mapped LEDs sit 40–580 mm from the trunk at every height, with
  no clean cone taper.
- **Map:** only 233 of 1000 LEDs are mapped (z −120 … 2000 mm) until
  calibration and remapping. The rest hold a sentinel position. The engine must
  degrade gracefully and be developable without the full map (§11).
- **No heap allocation while running.** All pools are sized at boot.
- **No brightness/power limits** (221 W measured at full white is within the
  supply).

## 3. Answers to ARCHITECTURE.md §6 open questions

| Question | Answer |
|---|---|
| Normalized vs metric coordinates | **Metric (mm, float)** world space. Each LED also gets precomputed normalized helpers (height 0–1, angle around trunk, radius fraction) for effects that want them. |
| Evaluate at LED points vs voxel grid | **Evaluate directly at LED points** for v1. A voxel-grid "volume layer" is a later layer type for fire/fluid (§5.1), not a foundation. |
| Fixed-point vs float | **Float** (single precision, hardware FPU). No doubles in the frame path — the M33's FPU is single-precision only, so doubles are much slower. |

## 4. Concepts and hierarchy

```
Engine
 ├─ Clock ................ simulation time (never wall-clock; §10)
 ├─ Global forces ........ gravity, wind, swirl — shared by all modes
 ├─ Scene ................ what's playing now: a stack of mode slots
 │   ├─ Slot 0  ─ Mode instance ─ params, layers, entities, emitters,
 │   ├─ Slot 1  ─ Mode instance     rules, local forces, lifecycle
 │   └─ …          (each slot composites as a group: blend, opacity, mask)
 ├─ Master ............... output stage: lights on/off, brightness,
 │                          transitions, gamma (no scene content)
 └─ Show runner .......... (later) sequences scenes: loop, chain, shuffle
```

| Term | Meaning |
|---|---|
| **Layer** | Something that produces a color (plus alpha) per LED, composited with a blend mode. |
| **Entity** | A simulated object with a shape, color, and motion, drawn into an entity layer. |
| **Template** | A named entity recipe (shape, size, color, physics, lifetime) used when spawning. |
| **Emitter** | Spawns entities from a template at a rate, inside a region, with randomized velocity. |
| **Force field** | An external influence on entity motion (gravity, wind, swirl, attractor). |
| **Rule** | Trigger → optional conditions → list of actions (§7). How entities *interact*. |
| **Mode** | A reusable effect: declares its parameters and builds layers/entities/rules/forces. |
| **Mode instance** | A mode running in a slot, with its own parameter values and state. |
| **Scene** | A stack of mode instances plus their settings. Presets are saved scenes. |
| **Base scene** | The scene the tree reverts to and boots into. Always exists; can be any scene. |
| **Scene edit** | Any change to the live scene (set a layer property, set a parameter, put a mode in a slot, spawn an entity). The app, rules, lifecycle policies, and shows all change the scene this way (§9.5). |
| **Show** | (later) A sequence/graph of scenes with durations and transitions. |

## 5. Layers

Every layer has: enabled, opacity, blend mode (**normal**, **add**, **max**,
**multiply**, **replace**), and an optional **mask** — a shape (same shape
library as entities, §6.2) that limits where the layer shows, with soft edges.

### 5.1 Layer types

| Type | Output | Uses |
|---|---|---|
| **Solid** | One color everywhere (within its mask) | Base color; today's Region paint = a masked solid layer |
| **Pixel** | A stored color + alpha per LED | Today's single-LED/group commands; later, streamed frames from a phone or the Pi |
| **Field** | A function of LED position and time | Gradients, rainbow around the trunk, noise, palettes by height |
| **Entity** | The entities assigned to this layer | Everything that moves |
| **Volume** *(later)* | A coarse voxel grid sampled at each LED | Fire, smoke, fluid-like simulations |

**Persistence (trails):** an entity or pixel layer can keep its previous frame
multiplied by a decay factor instead of clearing — comet tails and motion blur
at the cost of one buffer (12 KB).

**Palettes** are a shared resource: gradient color maps that fields and
entities sample (by height, age, speed, random). Most "Christmas looks" are
palette choices.

### 5.2 Compositing order

Each slot composites its own layers bottom → top into a group result, then the
scene composites slots bottom → top (each slot has its own blend, opacity,
mask), then master. Working color is **linear float RGB**;
gamma, brightness, and (later) temporal dithering happen only in the master
stage, which removes today's visible steps at low brightness.

**Mapping of today's state:** today's controls become an ordinary built-in
mode, **Canvas** (§8.3) — background color → a solid layer; paint/secondary →
a pixel layer above it; Region paint → a masked solid layer. There is no
separate "manual" layer group. This resolves the deferred base-vs-secondary
color rework.

## 6. Entities

### 6.1 Storage

A single fixed pool (~256 entities, ~128 B each ≈ 32 KB). Plain structs, not
an ECS. Each entity has a **handle** (index + generation) that stays valid for
its life, so commands and rules can safely refer to it. Each mode instance has
an **entity quota** so one stacked mode can't starve the others.

Core fields:

- Identity: handle, owner (mode instance), template id, layer id.
- Shape: shape type + size parameters + orientation (for slabs/wedges/capsules).
- Appearance: color (or palette + sample source), brightness, edge falloff,
  optional color-over-life.
- Motion: position, velocity, mass, drag, restitution, per-force response
  weights, optional **surface constraint**, **kinematic** flag (moved by
  commands, not physics — reserved for phone control, §12).
- Interaction: collision group bits, lifetime/age, user counters.

### 6.2 Shapes

| Shape | Example |
|---|---|
| **Sphere** | Ball, spark, snowflake |
| **Slab** (plane with thickness) | Today's sweeps — a horizontal slab moving in z |
| **Shell** (sphere surface) | Ripple, firework burst front |
| **Capsule** (line segment) | Streak, comet body |
| **Wedge** (angle slice about the trunk) | Rotating lighthouse beam |

Each shape is a signed-distance function; brightness at an LED comes from the
distance through the entity's **falloff profile** (hard, linear, smooth,
gaussian). Soft edges are the default: LEDs are centimeters apart, so hard
edges make LEDs pop on/off as things move; soft edges read as continuous
motion.

### 6.3 World model **[DECIDED]**

**3D world space in mm, with an optional per-entity surface constraint.** An
entity lights whichever LEDs fall inside it, so a ball dropping through the
tree lights a moving cross-section. The surface constraint projects an entity
onto a surface each step (e.g. the tree's outer envelope, so it "slides down
the outside"). The envelope is a height → radius profile fitted from the map.

### 6.4 Motion and forces

- Fixed simulation timestep (60 or 120 Hz) with substeps for fast entities;
  semi-implicit Euler integration.
- **Global forces** (shared, live-tunable from the app): gravity (direction and
  strength), wind (with gusts), swirl about the trunk, drag.
- **Local forces** (owned by a mode): point attractors/repellers, which can be
  attached to entities.
- Each entity weights each force (snow barely feels gravity, sparks feel it
  fully, a "balloon" feels it negatively).
- Future paintbrush tie-in: phone tilt can drive gravity's direction.

### 6.5 Collisions **[DECIDED, per review]**

- 16 collision groups; each entity is in one or more.
- A mode fills in a **response table** for pairs of groups:
  **ignore**, **overlap** (pass through, raise begin/end events),
  **bounce**, **stick**, **destroy A / B / both**.
- **Boundaries** (floor, top, trunk, outer envelope) have a per-entity
  response: bounce, stop, destroy, wrap (angle), or pass.
- Every collision, overlap, and boundary hit **raises an event**; the event
  can drive any action through rules (§7).
- Detection is brute-force pairs filtered by group mask (256 entities is
  cheap); bounding spheres first, exact shape tests second.

### 6.6 Rendering entities

For each entity layer, each entity is evaluated only against LEDs near it:
LED indices are pre-sorted by z, and an entity's z-extent selects a slice by
binary search. Estimated cost ~30 cycles per LED-entity evaluation; a few
hundred small entities fit in the frame budget. Full-tree shapes (slabs,
wedges) touch all LEDs but are few. A spatial grid is the upgrade if profiling
says so.

Unmapped LEDs (sentinel positions) are skipped by position-based layers and
entities; solid, pixel, and string-index effects still reach them.

## 7. Events, rules, and actions **[DECIDED: any event can drive any action]**

Interaction logic is data: a list of **rules**, each
`trigger → conditions → actions`. Modes declare their rules; C++ modes can
also handle events in code. Keeping it data-driven is what makes "link any
event to any action" uniform, and keeps a future app-authoring path open.

### 7.1 Triggers (events)

| Trigger | Carries |
|---|---|
| collision / overlap begin / overlap end (group A, group B) | both entities, contact point, relative speed |
| boundary hit (group, which boundary) | entity, point, speed |
| entity spawned / expired (group) | entity |
| timer (after T, every T) | — |
| count crosses threshold (group reaches N / drops to 0) | count |
| parameter changed | param id, value |
| external input (app button, later sensors) | input id, value |
| signal (named, raised by another rule — including other stacked modes) | payload |
| lifecycle (mode entered, cycle completed, ending, ended) | outcome |

### 7.2 Conditions

Optional filters: probability, cooldown per rule, max fire count, group
count below/above N, parameter comparisons, entity property tests (speed,
color, age).

### 7.3 Actions

| Action | Example |
|---|---|
| **spawn** (template, count, at event point/entity/region, velocity from event + spread) | Two balls collide → spawn a third ball; floor hit → 20 sparks |
| **destroy** (event entity A/B/both, group) | Absorb |
| **set property** (color, velocity, size, group, layer, template) | Mix colors on overlap; change group after first bounce |
| **impulse** | Kick entities away from a burst |
| **emitter** start / stop / set rate | Start snow after the finale |
| **force** set (global or local) | Gravity flips |
| **layer** set (color, opacity, mask) | Flash the background |
| **param** set / counter add | Score, escalation |
| **signal** (named, visible to other modes) | Fireworks mode tells the rainbow mode to pulse |
| **mode lifecycle**: end this mode (with an outcome), start/replace a mode in a slot, next scene, revert to base | Collision causes a mode transition |

Action values can be constants, random ranges, or taken from the event
(`a.color`, `mix(a.color, b.color)`, `a.velocity`, contact point).

### 7.4 Runaway protection

Rule chains can explode (ball spawns ball spawns ball…). Safeguards:

- Events raised by actions are **queued for the next tick**, never processed
  recursively in the same tick — no infinite loops within a frame.
- Per-mode entity quota; spawns beyond it fail and are counted.
- Per-tick action budget; per-rule cooldowns.
- Counters for failed spawns and budget hits appear on the app's Debug page,
  and the simulator flags them (§11).

## 8. Modes and stacking **[DECIDED: modes can be stacked]**

### 8.1 What a mode is

A mode declares:

- **Metadata:** id, name, category.
- **Parameters:** typed (color, number with range, choice, bool, palette),
  with defaults. The app builds controls from this description.
- **Setup:** layers, templates, emitters, local forces, response table, rules.
- **Code hooks (optional):** per-tick logic and event handling in C++ for
  things rules can't express.
- **Cycle definition (optional):** what counts as one cycle (e.g. one launch
  pass), used by lifecycle end conditions.

Most modes are expected to be mostly data (layers + emitters + rules) with
little code. v1 modes are compiled in; the data shape keeps a future path open
to modes saved from the app without reflashing.

### 8.2 Slots

The scene is a stack of up to ~4 **slots**. Each holds one mode instance and
composites as a group with its own blend, opacity, and mask — snow in slot 1
with blend *add* over a rainbow field in slot 0.

- **Isolation by default:** a mode's entities, groups, rules, and local forces
  are private to its instance. Global forces apply to all.
- **Cross-mode interaction [DECIDED]:** starts with **signals** (§7.3) only.
  Shared collision groups (snow settling on another mode's balls) are a
  possible later extension.
- The same mode can run in two slots with different parameters.

### 8.3 Manual control is not a special case **[DECIDED]**

Manual control uses only the engine's normal parts; there is no carve-out.
"Painting" is either editing a static layer (solid, masked solid, pixel) or
creating entities — through ordinary scene edits (§9.5), like everything else.

- **Canvas** is a built-in mode whose layers are a background solid, a masked
  solid (region paint), and a pixel layer (single LEDs / groups). Its
  parameters are those colors and the mask. It can sit in any slot, or several
  slots, like any other mode — e.g. a Canvas over a snow mode, or snow over a
  Canvas.
- Lights on/off and brightness stay in the master stage: they control
  *output*, not scene content.
- Existing protocol messages (FILL, BASE, SINGLE_LED, GROUP, VOL_*) are
  translated into ordinary layer edits on a **target layer address** (slot +
  layer). The default target is the base scene's Canvas. If the live scene has
  no layer at that address, the command is rejected (error ACK + event log),
  not special-cased.
- Later, painting with entities (the phone paintbrush, drawing on a picture of
  the tree) is spawning/moving entities in a slot's entity layer (§12).

## 9. Lifecycle: loop, chain, revert **[DECIDED: all three required]**

### 9.1 Mode instance lifecycle

```
 entering ──► running ──(end condition)──► ending ──(drained)──► ended ──► outcome policy
 (intro/        │                           (outro: stop emitters,
  fade in)      └─ cycle completed events    let entities finish)
```

**End conditions** (any combination; none = runs forever):

- duration (sim time),
- N cycles completed,
- a rule's **end mode** action (optionally with an **outcome** name,
  e.g. `"collided"`),
- drained (all its entities gone after emitters stop — "the fireworks are over").

**Outcome policy** — what the slot does when its instance ends. It can depend
on the outcome name, which makes a small state graph:

| Policy | Result |
|---|---|
| **loop** | Restart the same mode (N times or forever) |
| **chain** | Start another mode (with parameters) in this slot, with a transition |
| **revert** | Return the slot to the base scene's mode for that slot (or empty) |
| **remove** | Empty the slot; the stack below shows through |
| **hold** | Freeze the last frame of the slot until something else changes it |

Example: `fireworks` → on `"finale_done"` chain to `snowfall` → after 5 min
revert. Example: `pong` → on `"collided"` chain to `burst` → loop back to
`pong`.

### 9.2 Scene lifecycle

A scene has the same end conditions (duration, a designated slot ending, a
rule action) and the same policies at the whole-stack level: **loop**,
**next scene**, **revert to base**, **hold**. A **show** (later) is simply a
list of scenes whose policies point to each other, plus ordering options
(sequential, shuffle) and a loop setting.

### 9.3 Base scene **[DECIDED]**

- Always exists; the tree boots into it and every *revert* returns to it.
- **Can be any scene** — a stack of any modes with any parameters and
  lifecycle policies.
- v1 default: the equivalent of today — one slot holding Canvas with the
  current boot colors.
- Becomes persistable when the persistence work lands ("save the live scene as
  the base scene").

### 9.3.1 One scene description format

Because the base scene, presets, `SCENE_SET`, shows, and flash storage all
hold arbitrary scenes, they share **one serializable scene description**:
slots → mode id + parameter values + blend/opacity/mask + lifecycle policy,
plus global forces. Modes stay in firmware; the description only references
them by id and supplies parameters. It carries a version number so stored
scenes survive firmware updates that add parameters (unknown → ignored,
missing → default).

### 9.4 Transitions

Used when a slot or scene changes: **cut**, **crossfade** (snapshot the last
output of the slot/scene — 12 KB — and blend over a duration), **fade through
black**, later **wipe** (a moving shape used as a mask — the engine rendering
its own transition).

### 9.5 Everything changes the scene the same way **[DECIDED]**

The app has no special override path. App commands, rule actions, lifecycle
policies, and the show runner all produce the same **scene edits** against the
one live scene, applied in order at the start of a tick.

- An app edit changes the live scene immediately, like any other edit.
- A running show isn't paused by an edit. Its next step replaces the scene as
  its policy says. Stopping or pausing a show is itself an ordinary lifecycle
  command, which the app can offer as a button.
- Painting while a scene runs works by editing a layer that exists in it (a
  Canvas slot) or by adding one, e.g. putting Canvas into an empty slot.

## 10. Time

All engine time is **simulation time** advanced by `engine.step(dt)`. Timers,
durations, lifecycles, and randomness all run on it. Nothing in the engine
reads the wall clock. On the Pico the frame loop drives it at real time; the
simulator drives it at any speed (§11). Random numbers come from seeded
per-engine PRNGs.

Wall-clock behavior (e.g. "turn on at dusk") belongs to a later scheduler
*outside* the engine that issues ordinary commands.

## 11. Host simulator **[DECIDED]**

The engine is **portable C++20** (matching the firmware) with no Pico SDK dependencies, behind a small
platform interface (logging, LED geometry input, output sink). It builds into
the firmware and into host tools on Windows.

### 11.1 Tools

- **`neotree_sim` (headless CLI):** run a scene/show for any sim duration as
  fast as the PC can, e.g. `--scene fireworks --duration 8h --seed 42`.
  Options for event/lifecycle trace output, recording frames, and running
  checks. Simulation and rendering are separable, so lifecycle-only runs can
  skip rendering (or render every Nth frame) and go much faster still.
- **Viewer:** interactive 3D view of the tree's LEDs with play/pause/step,
  **speed from 0.1× to 1000×**, jump to a time, and debug overlays (entity
  shapes, velocities, forces, collision contacts, slot boundaries). Proposed
  on raylib (a single C library, easy 3D, builds with CMake).
- **Unit tests** for the engine core, run on the host.

### 11.2 Faster than real time

The simulator never sleeps: it steps the engine back-to-back. A PC should run
typical scenes many thousands of frames per second, so hours of a looping show
take seconds to minutes. Uses:

- Verify long loops/chains/shows actually cycle as intended and never get
  stuck.
- Catch slow leaks: entity count creep, quota exhaustion, runaway rules.
- **Checks** reported at the end of a run: pool/quota exhaustion, failed
  spawns, budget hits, stuck lifecycles, dark-tree periods, peak power
  estimate (informational only).

### 11.3 Geometry for development

The simulator (and firmware, until remapping) uses the **real 233 mapped
positions plus synthetic positions** for the rest, generated to match the
tree's height and radius distribution. Once calibration and remapping land,
both switch to the full map.

### 11.4 Determinism and Pico fidelity

- Same seed + same command log → same result within a platform.
- Bit-identical results across PC and Pico are **not** guaranteed (float
  rounding differs), so chaotic scenes may drift in detail, not in behavior.
- Frame *cost* must be measured on the Pico. The simulator reports work
  counters (LED-shape evaluations, pair tests, actions per tick) which we
  calibrate against Pico timing to flag scenes likely to exceed the budget.
- Later: record a command log on the Pico (from the Debug page) and replay it
  in the simulator.

## 12. Control protocol (sketch)

**v1:**

- `MODE_LIST` / `MODE_DESCRIBE` → JSON (like STATUS): modes, parameters,
  types, ranges, defaults.
- `SCENE_SET` (stack of slot → mode + params + blend/opacity),
  `SLOT_SET_MODE`, `SLOT_CLEAR`, `PARAM_SET` (slot, param, value).
- `LAYER_SET` (slot, layer, property, value) — the generic layer edit that
  painting uses.
- `LIFECYCLE` commands: next, revert to base, restart, show stop/pause/resume,
  save live scene as base.
- `FORCE_SET` (global forces), `INPUT` (app buttons as rule triggers).
- Existing messages keep working, translated into `LAYER_SET`-style edits on
  the target layer address (§8.3).
- Scene state becomes the single source of truth, pushed to all connected
  phones on change (ties into the multi-phone sync item).

**Later — designed for now, not built in v1:**

- **Direct entity control:** `ENTITY_SPAWN` / `ENTITY_UPDATE` / `ENTITY_KILL`
  by handle, with an owning client and a lease — a phone's entities are
  cleaned up if it disconnects. Kinematic entities are moved by the phone but
  still collide and raise events. The v1 entity struct reserves the owner and
  kinematic fields.
- **Stream channel** for high-rate input (paintbrush pose samples, pixel
  frames), likely UDP.

## 13. Open questions

None outstanding. Settled on review (2026-09-24):

1. **Cross-mode interaction:** signals first (§8.2).
2. **Manual control:** no carve-out — painting is layer edits or entities,
   through ordinary scene edits (§8.3, §9.5).
3. **Base scene:** starts as today's equivalent (Canvas), but can be any scene
   (§9.3).
4. **App scope:** the end goal is full scene configuration in the app, built
   as the engine is ready. **Engine first**: get it running and shake out
   fundamental issues before building app features on top (§14).

## 14. Build order

The ordering is **engine first**: M1–M4 build and prove the engine on the
simulator and the tree before app features are built on it. The app gains
scene configuration incrementally from M5 onward, toward full scene
configuration (stack editing, layer/entity/rule settings, presets, shows).

| # | Milestone | Proof |
|---|---|---|
| M1 ✅ | Engine skeleton (portable library), host build, headless CLI, viewer skeleton, LED geometry module (real + synthetic positions) | Viewer shows the tree's point cloud; CLI runs an empty scene at thousands of fps |
| M2 | Compositor: solid, pixel, field layers; masks; blends; master stage. Canvas mode; base scene = Canvas; existing messages translated to layer edits | Tree looks and behaves exactly as today; frame timing on the Debug page |
| M3 | Entities: shapes, falloff, integration, global forces, boundaries, surface constraint, z-culling | Gravity and launch sweeps recreated as entity demos, on the tree and in the sim |
| M4 | Collision groups, response table, events, rules/actions, templates, emitters, runaway protection | Snow and fireworks demos; ball-collision spawn chain stays bounded |
| M5 | Modes, slots (stacking), lifecycle (end conditions, loop/chain/revert/remove/hold), transitions, scene description format, base scene as any scene; describe/select/param/layer protocol; app: mode picker and parameters | Stacked snow-over-rainbow; a chained scene verified over hours of sim time |
| M6 | Shows, persistence of base scene and presets, state push to all phones; app: stack editing and presets | A holiday show loops unattended |
| M7 | Direct entity control from the phone, stream channel, paintbrush | Flick a ball from the phone into the tree |
| M8+ | App: full scene configuration (layers, entities, rules, forces, lifecycle), built as needed | A new effect built entirely from the app |

## 15. Progress log

### M1 — done 2026-09-24

- `engine/`: `Engine` (advance/run_ticks/render), `SimClock` (exact fixed
  ticks, no drift), `Rng` (xoshiro128**, pinned sequence), `Pool` (handles
  with generations), `LedGeometry` (cartesian/cylindrical/normalized, z-sorted
  index, `in_z_range`). 23 unit tests.
- `sim/`: `neotree_sim` (headless), `neotree_view` (raylib), position file from
  `mapping/generate_sim_positions.py`. Built with WinLibs GCC via
  `tools/build_sim.ps1`; see `sim/README.md`.
- Firmware: the engine links in (`firmware/src/neo_tree_engine.cpp`), builds
  its geometry from the stored map at boot and on map reloads, and runs every
  frame with its output discarded. Timing is in the status JSON under
  `engine`.

Measured:

| | |
|---|---|
| Firmware RAM (bss) | 140 KB → 183 KB of 520 KB (+43 KB: geometry 31 KB, frame 12 KB) |
| Firmware code | +5.7 KB |
| Geometry build on the Pico (1000 LEDs, 233 positioned) | ~66–69 ms, at boot only |
| Empty tick / empty render on the Pico | ~3 µs / ~52 µs |
| Host: 8 h of sim time, rendering every frame | 0.13 s wall (~220,000× real time) |
| Host: 8 h of sim time, no rendering | 3 ms wall |

**Found, to investigate at the start of M2:** core0 is periodically
interrupted or stalled. The existing LED frame prep (normally ~148 µs) peaks at
1.2–2.2 ms in most 5 s windows, and the engine's timing caught outliers of 17
and 42 ms (15 frames over 2 ms in the first 75 s after boot). The engine code
itself takes no locks and does no I/O, so these are time spent elsewhere. It's
invisible with static colors but would show as stutter once the engine drives
animation.

## 16. Memory estimate

| Item | Size |
|---|---|
| LED geometry (xyz, cylindrical, normalized, z-sort index) | ~40 KB |
| Entity pool (256 × ~128 B) | ~32 KB |
| Working framebuffer (float RGB) | 12 KB |
| Persistence buffers and transition snapshots (up to ~6 × 12 KB) | ~72 KB |
| Rules, templates, emitters, layers, slots | < 16 KB |
| **Total** | **~170 KB of 520 KB** (to be checked against current usage in M1) |
