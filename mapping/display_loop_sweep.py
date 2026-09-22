"""
Fun, display-only loop: a colored band sweeps top to bottom, continuously,
a fresh random color each pass. The band is the secondary-color overlay,
and clear_outside_volume lets the base color show through everywhere
outside it - so the base color is set once at startup (--base-color)
rather than left as whatever's currently loaded, since that's commonly
black right now (left over from calibration/testing, not a real display
setting). Defaults to firmware's own verified green_____ preset
(dcm_rgb.cpp) rather than a guessed RGB triple - a guessed "warm amber"
looked wrong on the real hardware (likely voltage drop along the strings
affecting the R/G/B LEDs differently), where the firmware presets were
already tuned by eye against the real strings. white_____ (neutral) was
tried first and worked correctly but looked boring as a backdrop; green
is also thematically a reasonable "tree" color.

Two modes:
  --mode linear (default): constant velocity, one full pass per
      1/--hz seconds.
  --mode gravity: falls from rest at the top like a real object under
      gravity - position(t) = top - 0.5*g*t^2, so it starts slow and
      accelerates, matching real free-fall rather than a steady sweep.
      Frame timing is paced against real elapsed wall-clock time (not a
      fixed per-frame sleep), so the actual motion tracks the physics
      even if serial round-trip time varies frame to frame.

Real z range is pulled from global_estimates automatically (same as
full_sweep_sequence.py), so it covers whatever's currently mapped.

Usage (venv active):
    python3 display_loop_sweep.py
    python3 display_loop_sweep.py --hz 0.5 --width 200 --base-color 0 60 20
    python3 display_loop_sweep.py --mode gravity --gravity 2000
"""
import argparse
import random
import time

import full_sweep_sequence
import sweep_db
import sweep_demo
import neotree_serial as neoser


def run_linear_pass(color, range_min, range_max, width, step, delay):
    for pos in range(range_max, range_min - step, -step):
        hi = max(range_min + width, min(pos, range_max))
        lo = hi - width
        sweep_demo.send_volume_frame('z', lo, hi, *color)
        neoser.ser.reset_input_buffer()
        time.sleep(delay)


def run_gravity_pass(color, range_min, range_max, width, gravity_mm_s2, fps):
    """
    One falling pass: the band's top edge starts at range_max with zero
    velocity and accelerates downward per free-fall (y(t) = top -
    0.5*g*t^2) until it reaches the bottom. Frame times are computed from
    a fixed start time rather than accumulated via per-frame sleep(), so
    per-frame overhead (serial writes, retries) doesn't let the apparent
    motion drift from the real physics over the course of a pass.
    """
    height = range_max - range_min
    dt = 1.0 / fps
    t0 = time.time()
    t = 0.0
    while True:
        fallen = 0.5 * gravity_mm_s2 * t * t
        hi = range_max - fallen
        landed = hi <= range_min + width
        if landed:
            hi = range_min + width
        lo = hi - width
        sweep_demo.send_volume_frame('z', int(lo), int(hi), *color)
        neoser.ser.reset_input_buffer()
        if landed:
            break
        t += dt
        sleep_time = (t0 + t) - time.time()
        if sleep_time > 0:
            time.sleep(sleep_time)
    return t


def run_launch_pass(color, range_min, range_max, width, gravity_mm_s2, fps):
    """
    One launch-and-fall pass: an object launched from the bottom with
    just enough initial velocity for its apogee (v=0) to land exactly when
    the rendered band reaches the top, then falls back down under the same
    gravity. Total flight time is 2*v0/g - by symmetry, exactly double
    run_gravity_pass's one-way fall time for the same effective height/
    gravity.

    The band is centered on the object's instantaneous height and clamped
    to stay within [range_min, range_max] - which means the center only
    actually has (height - width) of real travel room before it saturates
    at a boundary, not the full height. v0 is calibrated against that
    effective height, not the raw one: calibrating against the raw height
    (an earlier version of this) made the *true* unclamped apogee sit
    above range_max, so the center - and therefore the rendered band -
    saturated at the top boundary for a whole window of time approaching
    and leaving the peak (confirmed by testing: ~0.74s frozen at an
    identical position out of a ~3.0s flight, not just slow motion).
    Calibrating against (height - width) instead makes the true apogee
    land exactly on the clamp boundary, so clamping only ever binds at
    that single zero-velocity instant.
    """
    height = range_max - range_min
    effective_height = max(0.0, height - width)
    v0 = (2 * gravity_mm_s2 * effective_height) ** 0.5
    t_total = 2 * v0 / gravity_mm_s2
    dt = 1.0 / fps
    t0 = time.time()
    t = 0.0
    while True:
        y = v0 * t - 0.5 * gravity_mm_s2 * t * t
        done = t >= t_total
        y = 0.0 if done else max(0.0, min(effective_height, y))
        center = range_min + width / 2 + y
        center = max(range_min + width / 2, min(range_max - width / 2, center))
        lo, hi = center - width / 2, center + width / 2
        sweep_demo.send_volume_frame('z', int(lo), int(hi), *color)
        neoser.ser.reset_input_buffer()
        if done:
            break
        t += dt
        sleep_time = (t0 + t) - time.time()
        if sleep_time > 0:
            time.sleep(sleep_time)
    return t


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--db', default=sweep_db.DEFAULT_DB_PATH)
    parser.add_argument('--mode', choices=['linear', 'gravity', 'launch'], default='linear')
    parser.add_argument('--hz', type=float, default=1.0,
                         help="(--mode linear only) full top-to-bottom passes per second")
    parser.add_argument('--width', type=int, default=None,
                         help="band width in mm (default: 1/8 of the real z span)")
    parser.add_argument('--steps', type=int, default=15,
                         help="(--mode linear only) frames per pass - more is smoother motion but "
                              "needs a faster serial round-trip to hold the target Hz")
    parser.add_argument('--gravity', type=float, default=2000.0,
                         help="(--mode gravity/launch only) acceleration in mm/s^2 - real gravity is "
                              "9800, but that clears a real tree's height in well under a second; the "
                              "default gives a ~1.5s fall (gravity mode) or ~3s round trip (launch "
                              "mode) over a ~2.2m tree, visibly accelerating without being over "
                              "before it registers")
    parser.add_argument('--fps', type=float, default=12.0,
                         help="(--mode gravity/launch only) target frame rate during the motion")
    parser.add_argument('--range-min', type=int, default=None)
    parser.add_argument('--range-max', type=int, default=None)
    parser.add_argument('--base-color', type=int, nargs=3, default=[0, 140, 0], metavar=('R', 'G', 'B'),
                         help="set once at startup (default: (0,140,0), the RGB values of "
                              "firmware's own green_____ preset (dcm_rgb.cpp) - user-verified "
                              "visually pleasing on the real strings, unlike a guessed value. An "
                              "earlier guessed 'warm amber' (255,147,41) looked wrong on the real "
                              "hardware - likely voltage drop along the strings affecting the "
                              "R/G/B LEDs' apparent color differently, not something a plausible-"
                              "looking digital RGB triple accounts for; white_____ (80,80,80) "
                              "worked correctly but looked boring as a backdrop)")
    args = parser.parse_args()

    ranges = full_sweep_sequence.real_ranges(args.db)
    range_min = args.range_min if args.range_min is not None else ranges['z'][0]
    range_max = args.range_max if args.range_max is not None else ranges['z'][1]
    width = args.width if args.width is not None else max(30, (range_max - range_min) // 8)
    step = max(1, (range_max - range_min) // args.steps)
    delay = 1.0 / (args.hz * args.steps)

    error = neoser.open_neotree_serial('/dev/ttyACM0', baudrate=115200, timeout=0.2)
    if error:
        print(error)
        return
    time.sleep(0.3)
    neoser.ser.read(neoser.ser.in_waiting or 1)

    neoser.write_tree_all_led_base(neoser.ser, *args.base_color)
    time.sleep(0.2)
    neoser.ser.read(neoser.ser.in_waiting or 1)

    print(f"Base color set to rgb{tuple(args.base_color)}")
    if args.mode == 'linear':
        print(f"Display loop [linear]: z range=[{range_min},{range_max}] (top-to-bottom) width={width} "
              f"step={step} target={args.hz}Hz/pass ({args.steps} frames, delay={delay:.3f}s) "
              f"- Ctrl+C to stop")
    elif args.mode == 'gravity':
        expected_fall_s = (2 * (range_max - range_min) / args.gravity) ** 0.5
        print(f"Display loop [gravity]: z range=[{range_min},{range_max}] (top-to-bottom) width={width} "
              f"gravity={args.gravity}mm/s^2 fps={args.fps} expected_fall~={expected_fall_s:.2f}s "
              f"- Ctrl+C to stop")
    else:
        effective_height = max(0.0, (range_max - range_min) - width)
        expected_flight_s = 2 * (2 * effective_height / args.gravity) ** 0.5
        print(f"Display loop [launch]: z range=[{range_min},{range_max}] (bottom -> apogee at top -> "
              f"bottom) width={width} gravity={args.gravity}mm/s^2 fps={args.fps} "
              f"expected_flight~={expected_flight_s:.2f}s - Ctrl+C to stop")

    pass_count = 0
    t_start = time.time()
    try:
        while True:
            color = (random.randint(0, 255), random.randint(0, 255), random.randint(0, 255))
            pass_count += 1
            t0 = time.time()
            if args.mode == 'linear':
                run_linear_pass(color, range_min, range_max, width, step, delay)
            elif args.mode == 'gravity':
                run_gravity_pass(color, range_min, range_max, width, args.gravity, args.fps)
            else:
                run_launch_pass(color, range_min, range_max, width, args.gravity, args.fps)
            pass_time = time.time() - t0
            print(f"  pass {pass_count}: color=rgb{color} actual={pass_time:.2f}s "
                  f"(avg over run={(time.time()-t_start)/pass_count:.2f}s)")
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        neoser.cleanup_serial()


if __name__ == "__main__":
    main()
