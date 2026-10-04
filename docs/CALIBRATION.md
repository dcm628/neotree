# Mapping camera calibration — plan and notes

**Status (2026-10-03): the replacement board has arrived. The plan is now
phased mapping** (next section). The earlier single-distance plan below is
kept for its background: the board's limits, why the board alone isn't
enough, and the tooling built so far (commit 8332c4b).

## Phased mapping (agreed approach, 2026-10-03)

The board can't be detected beyond about 1.2 m, but the top section of the
tree is small enough to map from closer than that.
- **Phase 1:** map the top section from all angles at close range, at high
  accuracy.
- **Phase 2:** map the whole tree from further back, using the best phase-1
  LEDs as anchors.

Layout numbers come from a coverage simulation:
- tree surface radius = the current map's 85th percentile by height, about
  300–340 mm above 1.2 m;
- the tree's top at about 2.0 m;
- portrait cameras: 68.5° vertical by 41.9° horizontal field of view, focal
  length about 940 px.

```
        Phase 1 (top, 1.1 m)                Phase 2 (whole tree, 2.3 m)
                 S1                                     S1
           S6          S2                     S6                   S2
                (tree)                                 (tree)
           S5          S3                     S5                   S3
                 S4                                     S4
   6 stations, 60 degrees apart. Each sweep runs both pylons at
   opposite stations: S1+S4, then S2+S5, then S3+S6.
```

### Phase 1 — the top section (z ≈ 1.2–2.0 m), close range

- **Stand-off:** 1.1 m, trunk to cameras, measured horizontally.
  - Both cameras see 100% of the near half of the section.
  - Every LED is 0.69–1.24 m away, inside the board's detection range, so
    the stereo calibration is fitted across the volume it's used in.
  - At 1.0 m, 93% of the near half is seen; at 0.9 m, 79%.
- **Each pylon:**
  - bottom camera at about 1.30 m, top at about 1.90 m (592 mm apart);
  - both tilted about 15° toward the section's centre (1.6 m high at the
    trunk): the bottom one up, the top one down.
- **Stations:** 6, 60° apart; 3 sweeps with the two pylons at opposite
  stations. Each LED is then seen by 2–3 stations.
- **Expected accuracy:**
  - about 0.7 mm from pixel noise at 1.1 m, plus about 1.0 mm per 0.5 mrad
    of stereo angle error;
  - about 1 mm once the stations are joined in a bundle adjustment.
- **Focus:** set it for about 1 m. Focus 30 was chosen for 2.5 m, and the
  intrinsics depend on focus. Pick the value by board sharpness at 1.1 m,
  and calibrate each camera's intrinsics at that focus.
- **Calibration:**
  - intrinsics any time, at the phase-1 focus;
  - stereo per pylon after aiming;
  - moving a whole pylon between stations keeps the stereo calibration
    valid, but check it at each station with a 2-minute board capture
    compared against the calibration, to catch a bumped camera.
- **Anchor selection.** An LED becomes an anchor when it is:
  - seen by 2 or more stations;
  - low in bundle-adjustment residual;
  - within the wiring limits of its mapped neighbours (100 mm, or 300 mm at
    a strand joint);
  - estimated to about 2 mm or better.
- **Optional:** the board laid flat on the floor in one station's view, to
  give the true horizontal.

### Phase 2 — the whole tree, 2.3 m, anchored

- **Stand-off:** 2.3 m.
  - Cameras at about 0.65 m and 1.25 m, tilted about 7° toward the middle
    of the tree's height (0.94 m).
  - Both cameras see 100% of the near half, 1.7–2.45 m away (at 2.0 m: 98%).
- **Stations:** 6, 60° apart, 3 sweeps.
- **Focus:** the far focus, with intrinsics calibrated at it. Re-fit the
  stereo at near range as a starting value.
- **Anchors fix each station.** Solve each camera's pose from the anchors
  it sees (perspective-n-point), directly in the anchor frame.
  - This replaces the stereo angle the board can't measure at 2.3 m with
    known geometry at the working distance.
  - At 2.4 m a 0.5 mrad angle error would cost about 5 mm of depth; this
    removes it.
- **Then a bundle adjustment** over all phase-2 stations:
  - anchors held to their phase-1 uncertainty;
  - wiring limits as soft constraints;
  - a robust loss for bad blobs.
- **Expected accuracy:**
  - LEDs seen from 2+ stations are triangulated across stations: 60°
    apart, 2.3 m baselines, about 0.9 mm from pixel noise;
  - LEDs seen from one station fall back to that pylon's anchor-corrected
    stereo: about 3–5 mm.
- **Checks:**
  - anchors re-observed in phase 2 vs phase 1;
  - the board-corner check at near range;
  - wiring consistency;
  - the tree's measured height.

### What to build (in order)

1. **Intrinsics per camera and focus:** `intrinsics_<serial>_f<focus>.json`.
   Plus:
   - a focus finder: board sharpness vs focus at a set distance;
   - a board-range check: how far it's detected with the real cameras.
2. **Sweep records:** camera serials, focus, phase and station per sweep.
   Calibrated cameras used in capture and re-triangulation, with the tilt
   fit skipped.
3. **A station check:** a short board capture compared with the pylon's
   stereo calibration.
4. **Bundle adjustment across stations,** tested on synthetic data first,
   and anchor selection.
5. **Phase 2:** the per-camera anchor pose fit, then the anchored bundle
   adjustment.
6. **The tree frame and output:** trunk axis and up direction, the wiring
   outlier filter, then the firmware header.

## Why calibrate

The sweeps triangulated LEDs at 24–45 mm RMS, and multi-frame averaging
proved that's systematic bias, not noise. The geometry model
(`mapping/pylon_geometry.py`) assumed:
- each camera's focal length from its spec'd field of view (78°);
- no lens distortion;
- the two cameras of a pylon exactly parallel and 592 mm apart;
- plus a small top-camera rotation fitted per sweep from the LED data.

The user's priority is accuracy over speed: "The results are useless if
they aren't accurate."

## The rig (as of 2026-09-25)

| Pylon | Top camera | Bottom camera |
|---|---|---|
| A | /dev/video0 — C920, serial 846EA2EF | /dev/video2 — C920, serial 1D52796F |
| B | /dev/video4 — **C920e** (a different model), serial 39BF05FF | /dev/video6 — C920, serial BBD479AF |

- **Resolution:** sweeps request 1208×680, and the driver gives
  **1280×720**. Calibration must be done at 1280×720.
- **Focus:** manual **focus 30** (V4L2 units). The intrinsics are only
  valid at this focus.
- **Mounting:** the cameras are mounted portrait. Raw image +x is world
  up, and raw +y is the camera's right.
- **Identity:** cameras are identified by USB serial, not /dev/video number.
  The numbers can change between boots; the serials can't.
- On 2026-09-25 the cameras were pointing at the walls, so the rig had been
  moved or re-aimed since sweep 20.

## The board, and its limit

The ordered board (FoamCorePrint, `mapping/calibration_board.py`):
- 15×10 squares of 20 mm, 15 mm markers, DICT_4X4_100;
- 300×200 mm overall, 3 mm aluminium composite.

**It can only be detected out to about 1.2 m.** A marker is a 4×4 pattern
plus a border (6 cells across), and reading it needs about 2–3 px per cell.
With the cameras' focal length of about 940 px:

| Distance | 15 mm marker | Pixels per cell |
|---|---|---|
| 1 m | 14 px | 2.3 |
| 2 m | 7 px | 1.2 |
| 3 m | 4.7 px | 0.8 |

The tree is 2–3 m from the cameras. This size was a recommendation made
without doing this arithmetic.

**If a different size is possible:**
- To be detected at 2.5–3 m, markers need to be about 3× larger: roughly
  60 mm squares with 45 mm markers, e.g. a 10×7 grid of about 600×420 mm.
- 40 mm squares (about 400×300 mm) would reach about 2 m.
- Check the pixel arithmetic above for any candidate before ordering.
- Update `calibration_board.py` to match, including the dictionary. Verify
  the dictionary by decoding the vendor's preview image, as was done for this
  board.

## Why the board alone isn't enough

**Depth error at the tree scales with Z²/b, the distance squared over the
baseline:**
- Here that's 2.5² / 0.592 ≈ 10 m per radian.
- So a 1 milliradian (0.06°) error in the angle between a pylon's two
  cameras becomes about 9–10 mm of depth error.
- Small errors in each camera's optical centre (±2 px) act the same way.

**Synthetic test** (`mapping/test_calibration_synthetic.py`, rendering this
exact board for known cameras):
- The scripts recover the truth: focal lengths within about 0.2%, and the
  top camera's position within 1–2 mm.
- But because every pose both cameras saw was under about 1.2 m, LEDs at
  1.5–3 m still came out **7–15 mm off**, even with perfect pixel data.

**What doesn't fix it:** refining one pylon's stereo pose from its own LED
correspondences with an essential matrix. It diverged in testing, and for a
fundamental reason. With points all at roughly one distance, a rotation that
shifts every disparity equally leaves no signature in the matches. Only
known distances, or a second viewpoint, pin it down.

## The plan

### 1. Each camera's intrinsics — the board is fine for this
- Poses at 0.5–1.2 m, board filling a good part of the frame.
- Cover the whole view: centre, edges and corners.
- Vary the tilt, plus a spread of near and far.
- Focus stays locked at 30. Very close poses may be slightly soft, and the
  detector copes.
- Output: `calibration/intrinsics_<serial>.json`.

### 2. Each pylon's stereo pose — the board gives the starting point
- Poses seen by both cameras of a pylon, as far out as the board is detected.
- This gives the top camera's position and orientation relative to the
  bottom one: roughly right, but 7–15 mm off at the tree.
- It fixes the **scale**: the true baseline, about 592 mm.
- **It's valid only while neither camera on that pylon is re-aimed or moved
  on its rail.** So calibrate after the final aiming, and don't touch the
  cameras until the sweeps are done. Moving a whole pylon is fine.
- Output: `calibration/stereo_<pylon>.json`.

### 3. The real accuracy: a joint fit of all four cameras on the LEDs
This is the standard large-volume approach (bundle adjustment), and it needs
no new hardware.
- **The sweep:** both pylons watch from well-separated angles (ideally
  around 90°), so most LEDs are seen by 3–4 cameras.
- **The fit:** solve every camera's pose and every LED's position together,
  minimising reprojection error.
  - Intrinsics are held fixed from step 1. That's what stops it overfitting,
    unlike the old per-sweep tilt fit on guessed intrinsics.
  - Each pylon's baseline length is held at the board's value (the scale).
  - The step 2 poses are the starting point.
- **Why it works:** one pylon's weak direction (depth) is the other pylon's
  strong direction (sideways), so the equal-disparity ambiguity from the
  failed test is resolved.
- **Before building it:** test it on synthetic data first, like the
  calibration scripts: known cameras, a cone of about 500 LEDs, pixel noise.
  Check it recovers the geometry to millimetres.
- **Outliers:** use a robust loss (Huber, or reject by residual), since some
  LEDs will have bad blobs (reflections, neighbours bleeding in).

### 4. Check it against something independent, at the tree
- **Tape measure:** measure a few distances between recognisable LEDs, and
  the tree's height (about 2130 mm), and compare with the map.
- **Or a "wand":** two small lights on a rigid stick at a measured distance,
  moved around the tree during a capture. Every frame is a known-length check
  at the working distance; motion-capture systems calibrate large volumes
  this way. It needs two-blob detection in the capture.
- **Or known LED spacing:** if the strings have a fixed spacing, consecutive
  LEDs on straight runs give another check (not yet asked).

### 5. Optional, strong: put the pylons closer
- At about 1.5 m instead of 2.5 m, depth error drops about 2.8×, since it
  goes with Z².
- The board's range then reaches the near side of the tree.
- Each camera still covers about 2 m of height at 1.5 m (portrait, about 69°
  vertical field of view), and the two cameras together cover about 2.6 m.
- It needs more pylon positions around the tree, which the multi-sweep
  alignment already handles.

## Session day checklist

1. Pylons set up and **aimed at the tree for the mapping**. From here on,
   don't touch the cameras.
2. **Room lights on** (the board needs light; the sweeps need dark later).
3. Board clean and flat. Check the square size with a ruler or calipers
   against 20.0 mm (or the new board's size).
4. Run on the Pi (venv active, from `mapping/`):
   `python3 capture_calibration_images.py --out-dir calib_images/session1`
   - The tree shows the capture state:
     - dim blue: nothing seen;
     - amber: hold still;
     - purple: already captured there, move it;
     - green flash: captured;
     - white: done.
   - It prints what each camera still needs (e.g. "high to the camera's
     left").
   - About 15–20 minutes. Often hold the board where both cameras of a pylon
     see it, as far out as it's still detected.
5. `python3 calibrate_intrinsics.py --session calib_images/session1`.
   Expect well under 1 px RMS per camera. The field of view it reports
   replaces the assumed 78°.
6. `python3 calibrate_stereo.py --session calib_images/session1`.
   - Expect a baseline near 592 mm.
   - The board-corner check (mm error vs distance) is the accuracy up to
     about 1.2 m.
7. The sweep(s) for step 3, lights off, cameras untouched.

## Still to build (in order)

1. **Record camera serials per sweep.**
   - A new DB table, e.g. `pylon_cameras(sweep_id, pylon_id, top_serial,
     bottom_serial)`, written by `capture_sweep.py`.
   - A sweep's pixels can then always be re-solved with the right
     calibration.
2. **Use calibrated cameras when available.**
   - In `capture_sweep.py` and `retriangulate_sweep.py`, use
     `pylon_geometry.make_calibrated_pylon_cameras(bottom_serial, top_serial,
     w, h)` when calibration files exist.
   - Skip `fit_camera_tilt.py` in `process_sweep.py` for calibrated pylons.
   - Set `pylon_placements.calibration_source` to "charuco".
3. **The joint fit (step 3)**, synthetic test first, then on real sweeps.
4. **The independent check (step 4).**
5. **Remap.** Then regenerate the firmware position header from the new
   map. The DB's current `global_estimates` (sweeps 13 and 15) is provisional
   and must not be flashed.

## What's built (commit 8332c4b)

- **`capture_calibration_images.py`:**
  - captures all four cameras at once;
  - takes a pose only when the corners have moved under a pixel for several
    frames, somewhere new for that camera;
  - saves the frames of every camera seeing it still together, for stereo;
  - writes a manifest with serials, resolution and focus;
  - tracks coverage on a 3×3 grid plus near/far spread;
  - uses the tree as the status light (Solid mode in the top slot, removed
    at the end).
- **`calibrate_intrinsics.py`:** fits each camera from the manifest, drops
  outlier views and fits again, and reports the field of view.
- **`calibrate_stereo.py`:** fits each pylon with intrinsics fixed, then the
  board-corner accuracy check in mm by distance.
- **`pylon_geometry.py`:** `CameraModel` takes OpenCV intrinsics.
  `make_calibrated_pylon_cameras` converts the stereo fit into the
  portrait-mounted pylon frame (`CV_TO_LOCAL`). The self-test includes a
  calibrated round trip.
- **`test_calibration_synthetic.py`:** renders the board for known cameras
  and checks the whole chain. Run it on the Pi: the Pi's OpenCV is 5.0, and
  the desktop has none.
