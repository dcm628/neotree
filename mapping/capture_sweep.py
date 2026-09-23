"""
Runs one mapping sweep: walks LEDs one at a time, captures a centroid from
each active pylon's top/bottom cameras, triangulates a 3D position (in that
pylon's own local frame) per LED per pylon, and logs everything to
sweep_data.sqlite3 (see sweep_db.py for the schema/raw-vs-derived split).

Camera-to-pylon wiring isn't assumed fixed across sessions (USB
enumeration order can shift) - confirm ids with identify_cameras.py first,
then pass them explicitly. At least one pylon (both --pylon-*-top and
--pylon-*-bottom) is required; a second is optional.

No cross-sweep/cross-pylon alignment happens here - each pylon's solves
stay in its own local frame. That registration step is a separate,
later phase; this script's job ends at "here's what one pylon's stereo
pair could see this sweep."

Performance notes (why a sweep takes as long as it does, and what's tuned
here to cut that down without touching capture quality):
  - --dwell/--background-dwell auto-compute a minimum settle time from
    real physical constraints (see compute_min_dwell_s) instead of a
    guessed constant, unless explicitly overridden.
  - Retries are no longer unconditional - see capture_centroid_with_retry.
  - --gap-fill skips LEDs already well-covered in global_estimates, so
    repeat sweeps over a partially-mapped tree get proportionally faster
    as coverage grows.
  - --threaded processes pylons concurrently (one worker per pylon)
    instead of sequentially. A first attempt at this destabilized a
    pylon's USB hub chain badly enough to need a physical cable/port
    swap to recover - later USB topology inspection (lsusb -t) found
    the two pylons sit behind genuinely separate xHCI host controllers
    (no shared bandwidth contention), and root-caused the failure to
    that specific hub chain being marginal hardware at the time, not
    concurrent V4L2 access being inherently unsafe. Off by default;
    watch dmesg during the first run on any hardware that's had
    connection trouble before trusting it unattended.

Usage (venv active):
    python3 capture_sweep.py --pylon-a-top 0 --pylon-a-bottom 2 \\
        --pylon-b-top 4 --pylon-b-bottom 6 --notes "first full sweep, tree front"
"""
import argparse
import time
from concurrent.futures import ThreadPoolExecutor

import cv2

import neotree_camera as neocam
import neotree_serial as neoser
import pylon_geometry as geom
import sweep_db

# Empirically measured LED refresh loop rate (led_loop_counter heartbeat,
# established earlier this project): ~34.6Hz, i.e. ~29ms worst-case
# latency between a serial command landing and the next periodic
# write_string() refresh actually pushing it to the physical LEDs.
LED_REFRESH_MARGIN_S = 0.029


def compute_min_dwell_s(exposure_v4l2_units, fps, led_refresh_margin_s=LED_REFRESH_MARGIN_S, frame_margin_s=0.025):
    """
    Minimum reliable settle time before capturing a frame that's
    guaranteed to fully reflect a just-issued LED state change (lighting
    an LED, or turning everything off for a background frame):
      - led_refresh_margin_s: worst-case time for the change to actually
        reach the physical LEDs (see module docstring).
      - max(exposure, 1/fps): the frame we capture needs a full exposure
        window entirely *after* the change, or it partially blends the
        old and new state - but a UVC camera can never deliver a frame
        faster than its own negotiated frame period regardless of
        exposure (confirmed by testing: it silently clamps exposure down
        to fit 1/fps if you request more, so the frame *period* is the
        real floor, not the exposure value alone).
      - frame_margin_s: safety margin for USB/driver frame delivery
        latency and buffer-drain uncertainty beyond that.
    Paired with neocam.drain_for (continuously drains the buffer for the
    whole dwell, not a single flush read after a blind sleep) - see its
    docstring for why a single flush wasn't reliably enough.
    """
    exposure_s = exposure_v4l2_units * 100e-6
    frame_period_s = 1.0 / fps
    return led_refresh_margin_s + max(exposure_s, frame_period_s) + frame_margin_s


def build_pylon(pylon_id, top_id, bottom_id, width, height, spacing_mm, exposure, gain, fps):
    captures = neocam.initialize_video_capture([top_id, bottom_id])
    if captures is None:
        raise RuntimeError(f"pylon {pylon_id}: could not open camera ids {top_id}/{bottom_id}")
    neocam.set_camera_settings(captures, width=width, height=height, exposure=exposure, gain=gain, fps=fps)
    top_cap, bottom_cap = captures
    # The driver doesn't always honor the requested resolution (seen in
    # practice: 1208x680 requested, 1280x720 negotiated) - the geometry
    # model's focal length must match what's actually coming back, or
    # every triangulation is silently off.
    actual_width = int(top_cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    actual_height = int(top_cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    if actual_width != width or actual_height != height:
        print(f"pylon {pylon_id}: requested {width}x{height}, driver negotiated "
              f"{actual_width}x{actual_height} - using the actual size for triangulation")
    # Settle before trusting the exposure readback: confirmed by testing
    # that get(CAP_PROP_EXPOSURE) immediately after set() can echo back
    # the requested value even though the camera silently drifts it down
    # over the first few real frames to whatever 1/fps actually allows.
    neocam.drain_for([top_cap, bottom_cap], 0.3)
    actual_exposure = top_cap.get(cv2.CAP_PROP_EXPOSURE)
    actual_fps = top_cap.get(cv2.CAP_PROP_FPS)
    bottom_model, top_model = geom.make_pylon_cameras(actual_width, actual_height, spacing_mm=spacing_mm)
    return {
        "pylon_id": pylon_id,
        "top_id": top_id, "bottom_id": bottom_id,
        "top_cap": top_cap, "bottom_cap": bottom_cap,
        "top_model": top_model, "bottom_model": bottom_model,
        "width": actual_width, "height": actual_height,
        "exposure": actual_exposure, "fps": actual_fps,
    }


def capture_background_frames(pylon):
    """
    One all-LEDs-off frame per camera, captured fresh for THIS LED
    position (not once for the whole sweep) - ambient light (daylight
    through windows, in particular) can change meaningfully over a
    multi-minute sweep, and a stale background frame was confirmed by
    testing to cause near-total detection failure under daylight: a
    static glare/reflection spot won the single-blob detection every
    time regardless of which LED was actually lit, and broad daylight
    brightening tripped the multi-blob rejection almost everywhere.

    No flush read here - the caller drains every active camera's buffer
    for the full dwell period (see neocam.drain_for), which both
    satisfies the settling time and guarantees the buffer is empty, so
    this first real read is already fresh. A single flush read after a
    blind time.sleep() (the old approach) was confirmed via
    investigate_frame_timing.py to still often land on a stale,
    pre-transition frame - the camera's buffer can run several frames
    deep, and one flush doesn't reliably clear it.
    """
    return neocam.capture_frame(pylon["top_cap"]), neocam.capture_frame(pylon["bottom_cap"])


def capture_centroid_with_retry(cap, background, retries, threshold_value, retry_delay_s,
                                 retry_on_zero_blobs, retry_on_ambiguous):
    """
    Retries only when it's plausibly worth it, instead of always burning
    the full retry budget:
      - blob_count == 0 (nothing found): retried only if
        retry_on_zero_blobs is set. Nothing changes between attempts
        (exposure/gain/focus are all fixed - no settling to wait out),
        and 0 blobs almost always means real occlusion, a static
        geometric fact retrying won't fix. Off by default.
      - blob_count > 1 (ambiguous/multi-blob): retried if
        retry_on_ambiguous is set (default on) - more plausibly transient
        noise (a reflection, a compression artifact) worth re-sampling.
    Stops as soon as retrying stops being worth it, rather than sleeping
    out the rest of the budget regardless.

    The first attempt takes a single read with no flush (the caller
    already drained the buffer for the shared dwell period - see
    capture_background_frames). A retry drains for retry_delay_s instead
    of sleeping, for the same reason: guarantees the next attempt's read
    is fresh rather than landing back on stale backlog.
    """
    blob_count, blob_area = 0, 0.0
    attempt = 0
    for attempt in range(1, retries + 1):
        frame = neocam.capture_frame(cap)
        if frame is not None:
            cx, cy, blob_count, blob_area = neocam.find_single_blob_centroid(
                frame, background=background, threshold_value=threshold_value)
            if cx is not None:
                return cx, cy, blob_count, blob_area, attempt
        worth_retrying = (blob_count == 0 and retry_on_zero_blobs) or (blob_count > 1 and retry_on_ambiguous)
        if attempt >= retries or not worth_retrying:
            break
        neocam.drain_for([cap], retry_delay_s)
    return None, None, blob_count, blob_area, attempt


def capture_pylon_lit(pylon, top_bg, bottom_bg, retries, threshold_value, retry_delay_s,
                       retry_on_zero_blobs, retry_on_ambiguous):
    """Runs in a worker thread - one pylon's top+bottom capture+retry for the currently-lit LED."""
    results = {}
    for pos, cap, bg in (("top", pylon["top_cap"], top_bg), ("bottom", pylon["bottom_cap"], bottom_bg)):
        results[pos] = capture_centroid_with_retry(
            cap, bg, retries, threshold_value, retry_delay_s, retry_on_zero_blobs, retry_on_ambiguous)
    return results


def compute_led_positions(conn, start_led, count, gap_fill, max_cov_trace):
    candidates = list(range(start_led, start_led + count))
    if not gap_fill:
        return candidates
    well_covered = set(r[0] for r in conn.execute(
        "SELECT led_position FROM global_estimates WHERE cov_xx + cov_yy + cov_zz <= ?", (max_cov_trace,)))
    remaining = [led for led in candidates if led not in well_covered]
    print(f"Gap-fill: {len(candidates) - len(remaining)}/{len(candidates)} LEDs already well-covered "
          f"(cov_trace <= {max_cov_trace}) - skipping them, walking the remaining {len(remaining)}")
    return remaining


def record_pylon_observations(conn, sweep_id, pylon, i, obs_raw, pixel_sigma_px):
    obs = {}
    for pos, (cx, cy, blob_count, blob_area, attempts) in obs_raw.items():
        found = cx is not None
        sweep_db.record_raw_observation(
            conn, sweep_id, pylon["pylon_id"], pos, i, found,
            pixel_x=cx, pixel_y=cy, blob_count=blob_count,
            blob_area=blob_area, attempts=attempts)
        if found:
            obs[pos] = (cx, cy)

    if "top" in obs and "bottom" in obs:
        result = geom.triangulate_pylon_observation(
            pylon["bottom_model"], pylon["top_model"],
            obs["bottom"], obs["top"], pixel_sigma_px=pixel_sigma_px)
        sweep_db.record_session_solve(conn, sweep_id, pylon["pylon_id"], i, result)


def run_sweep(conn, sweep_id, pylons, led_positions, dwell_s, background_dwell_s,
              retries, pixel_sigma_px, threshold_value, retry_delay_s,
              retry_on_zero_blobs, retry_on_ambiguous, threaded=False):
    # threaded=True parallelizes the per-pylon background/lit captures
    # (one worker per pylon) across the shared serial commands (all-off,
    # single-LED-on), which stay strictly sequential since there's only
    # one physical tree/LED state both pylons observe at once. A prior
    # threaded attempt destabilized pylon A's USB hub chain badly enough
    # to need a physical port/cable swap to fully recover - root-caused
    # afterward (see 2025-09-22 session notes / git log) to that specific
    # hub chain being marginal hardware at the time, not concurrent V4L2
    # access being inherently unsafe: pylon A and B sit behind genuinely
    # separate xHCI host controllers (confirmed via lsusb -t), and
    # OpenCV's native V4L2 backend doesn't share state across
    # VideoCapture objects on different devices. Re-verify with dmesg
    # during/after a run before trusting this on hardware that's had
    # connection trouble.
    total = len(led_positions)
    all_caps = [cap for p in pylons for cap in (p["top_cap"], p["bottom_cap"])]
    executor = ThreadPoolExecutor(max_workers=len(pylons)) if threaded else None
    try:
        for walked, i in enumerate(led_positions, start=1):
            neoser.write_tree_all_led(neoser.ser, 3, 0, 0, 0)
            neocam.drain_for(all_caps, background_dwell_s)
            if threaded:
                bg_futures = {p["pylon_id"]: executor.submit(capture_background_frames, p) for p in pylons}
                backgrounds = {pid: f.result() for pid, f in bg_futures.items()}
            else:
                backgrounds = {p["pylon_id"]: capture_background_frames(p) for p in pylons}

            neoser.write_tree_single_led(neoser.ser, 1, i, 255, 255, 255)
            neocam.drain_for(all_caps, dwell_s)

            if threaded:
                lit_futures = {}
                for pylon in pylons:
                    top_bg, bottom_bg = backgrounds[pylon["pylon_id"]]
                    lit_futures[pylon["pylon_id"]] = executor.submit(
                        capture_pylon_lit, pylon, top_bg, bottom_bg, retries, threshold_value,
                        retry_delay_s, retry_on_zero_blobs, retry_on_ambiguous)
                for pylon in pylons:
                    obs_raw = lit_futures[pylon["pylon_id"]].result()
                    record_pylon_observations(conn, sweep_id, pylon, i, obs_raw, pixel_sigma_px)
            else:
                for pylon in pylons:
                    top_bg, bottom_bg = backgrounds[pylon["pylon_id"]]
                    obs_raw = capture_pylon_lit(pylon, top_bg, bottom_bg, retries, threshold_value,
                                                 retry_delay_s, retry_on_zero_blobs, retry_on_ambiguous)
                    record_pylon_observations(conn, sweep_id, pylon, i, obs_raw, pixel_sigma_px)

            if walked % 100 == 0 or walked == total:
                print(f"  {walked}/{total} LEDs walked (position {i})")
    finally:
        if executor:
            executor.shutdown(wait=True)


def print_summary(conn, sweep_id, led_count):
    summary = sweep_db.sweep_summary(conn, sweep_id)
    print(f"\n--- sweep {sweep_id} summary ({led_count} LEDs walked) ---")
    for pylon_id, counts in summary.items():
        print(f"  pylon {pylon_id}: solved={counts['solved']} "
              f"partial(1 camera only)={counts['partial_one_camera']} "
              f"no_centroid={counts['no_centroid']}")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--pylon-a-top', type=int)
    parser.add_argument('--pylon-a-bottom', type=int)
    parser.add_argument('--pylon-b-top', type=int)
    parser.add_argument('--pylon-b-bottom', type=int)
    parser.add_argument('--db', default=sweep_db.DEFAULT_DB_PATH)
    parser.add_argument('--width', type=int, default=1208)
    parser.add_argument('--height', type=int, default=680)
    parser.add_argument('--spacing-mm', type=float, default=geom.PYLON_CAMERA_SPACING_MM,
                         help="vertical distance between each pylon's top/bottom camera")
    parser.add_argument('--fps', type=int, default=10,
                         help="camera frame rate - directly caps the max real exposure (a UVC "
                              "camera silently clamps exposure down to fit 1/fps regardless of "
                              "what's requested, confirmed by testing at the driver's ~30fps "
                              "default: a requested 666 was actually being delivered as ~312). "
                              "Lower fps unlocks a longer real exposure at the cost of a longer "
                              "minimum frame period (see compute_min_dwell_s).")
    parser.add_argument('--exposure', type=int, default=666, help="manual exposure, V4L2 100us units")
    parser.add_argument('--gain', type=int, default=255, help="manual gain, 0-255")
    parser.add_argument('--start-led', type=int, default=0, help="first LED position to walk")
    parser.add_argument('--count', type=int, default=1000, help="how many LEDs to walk, starting at --start-led")
    parser.add_argument('--gap-fill', action='store_true',
                         help="skip LEDs already well-covered in global_estimates instead of "
                              "walking the full --start-led/--count range - see --max-cov-trace")
    parser.add_argument('--max-cov-trace', type=float, default=1000.0,
                         help="(--gap-fill only) a global_estimates LED with cov_xx+cov_yy+cov_zz "
                              "at or below this counts as already well-covered")
    parser.add_argument('--dwell', type=float, default=None,
                         help="seconds to wait after lighting an LED before capturing - default: "
                              "auto-computed from the camera's actual exposure setting plus LED "
                              "refresh/frame-delivery margins (see compute_min_dwell_s); pass an "
                              "explicit value to override")
    parser.add_argument('--background-dwell', type=float, default=None,
                         help="seconds to wait after turning all LEDs off before capturing that "
                              "position's background frame - default: same auto-computed value as "
                              "--dwell (the same physical constraints apply to an 'off' transition)")
    parser.add_argument('--retries', type=int, default=3, help="max capture attempts per camera per LED")
    parser.add_argument('--retry-delay', type=float, default=0.03,
                         help="seconds between retry attempts - short, since nothing changes "
                              "between attempts (exposure/gain/focus are fixed) beyond re-sampling "
                              "frame-to-frame noise")
    parser.add_argument('--retry-on-zero-blobs', action=argparse.BooleanOptionalAction, default=False,
                         help="retry when a camera finds nothing at all - off by default, since 0 "
                              "blobs almost always means real occlusion, not something a retry fixes")
    parser.add_argument('--retry-on-ambiguous', action=argparse.BooleanOptionalAction, default=True,
                         help="retry when a camera finds more than one blob (ambiguous) - on by "
                              "default, since this more plausibly indicates transient noise")
    parser.add_argument('--threshold', type=int, default=250)
    parser.add_argument('--pixel-sigma', type=float, default=1.0,
                         help="assumed 1-sigma centroid pixel noise, for the covariance estimate")
    parser.add_argument('--threaded', action='store_true',
                         help="capture both pylons' background/lit frames concurrently (one worker "
                              "per pylon) instead of sequentially - meaningfully faster with 2 "
                              "pylons, but a prior attempt destabilized a USB hub chain on hardware "
                              "that had a marginal cable/port at the time (see run_sweep's "
                              "docstring). Watch dmesg on the Pi during the first run on any "
                              "hardware that's had connection trouble before trusting this.")
    parser.add_argument('--notes', default=None)
    args = parser.parse_args()

    pylon_args = {
        "A": (args.pylon_a_top, args.pylon_a_bottom),
        "B": (args.pylon_b_top, args.pylon_b_bottom),
    }
    requested = {pid: ids for pid, ids in pylon_args.items() if ids[0] is not None or ids[1] is not None}
    for pid, (top_id, bottom_id) in requested.items():
        if top_id is None or bottom_id is None:
            parser.error(f"pylon {pid}: both --pylon-{pid.lower()}-top and "
                         f"--pylon-{pid.lower()}-bottom are required if either is given")
    if not requested:
        parser.error("at least one pylon (--pylon-a-top/--pylon-a-bottom) is required")

    error = neoser.open_neotree_serial('/dev/ttyACM0', baudrate=115200)
    if error:
        print(error)
        return
    neoser.write_tree_all_led(neoser.ser, 3, 0, 0, 0)
    time.sleep(0.25)

    pylons = []
    try:
        for pid, (top_id, bottom_id) in requested.items():
            print(f"Opening pylon {pid}: top={top_id} bottom={bottom_id}")
            pylons.append(build_pylon(pid, top_id, bottom_id, args.width, args.height, args.spacing_mm,
                                       args.exposure, args.gain, args.fps))

        dwell_s = args.dwell
        background_dwell_s = args.background_dwell
        if dwell_s is None or background_dwell_s is None:
            representative_exposure = pylons[0]["exposure"]
            representative_fps = pylons[0]["fps"]
            auto_dwell = compute_min_dwell_s(representative_exposure, representative_fps)
            if dwell_s is None:
                dwell_s = auto_dwell
            if background_dwell_s is None:
                background_dwell_s = auto_dwell
            print(f"Auto-computed dwell={auto_dwell:.3f}s from pylon {pylons[0]['pylon_id']}'s actual "
                  f"exposure={representative_exposure} (V4L2 100us units = {representative_exposure*0.1:.1f}ms) "
                  f"fps={representative_fps} (frame period {1000.0/representative_fps:.1f}ms) "
                  f"+ {LED_REFRESH_MARGIN_S*1000:.0f}ms LED-refresh margin + 25ms frame-delivery margin")

        conn = sweep_db.connect(args.db)
        try:
            while True:
                sweep_id = sweep_db.start_sweep(conn, notes=args.notes)
                for pylon in pylons:
                    sweep_db.record_pylon_placement(
                        conn, sweep_id, pylon["pylon_id"], pylon["top_id"], pylon["bottom_id"],
                        pylon["width"], pylon["height"], args.spacing_mm, calibration_source="nominal")

                led_positions = compute_led_positions(
                    conn, args.start_led, args.count, args.gap_fill, args.max_cov_trace)

                print(f"\nStarting sweep {sweep_id} ({len(led_positions)} LEDs, dwell={dwell_s:.3f}s, "
                      f"retries={args.retries} [zero_blobs={args.retry_on_zero_blobs} "
                      f"ambiguous={args.retry_on_ambiguous}])...")
                t0 = time.time()
                run_sweep(conn, sweep_id, pylons, led_positions, dwell_s, background_dwell_s,
                          args.retries, args.pixel_sigma, args.threshold, args.retry_delay,
                          args.retry_on_zero_blobs, args.retry_on_ambiguous, threaded=args.threaded)
                elapsed = time.time() - t0
                per_led = elapsed / len(led_positions) if led_positions else 0.0
                print(f"Sweep {sweep_id} done in {elapsed:.1f}s ({per_led:.3f}s/LED)")
                print_summary(conn, sweep_id, len(led_positions))

                answer = input("\nRelocate pylon(s) and run another sweep? [y/N]: ").strip().lower()
                if answer != 'y':
                    break
                input("Reposition camera(s) now, then press Enter to start the next sweep...")
        finally:
            conn.close()
    finally:
        neoser.write_tree_all_led(neoser.ser, 3, 0, 0, 0)
        for pylon in pylons:
            pylon["top_cap"].release()
            pylon["bottom_cap"].release()
        neoser.cleanup_serial()


if __name__ == "__main__":
    main()
