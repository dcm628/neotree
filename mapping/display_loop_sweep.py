"""
Fun, display-only loop: a colored band sweeps top to bottom, continuously,
a fresh random color each pass. The band is the secondary-color overlay,
and clear_outside_volume lets the base color show through everywhere
outside it - so the base color is set once at startup (--base-color)
rather than left as whatever's currently loaded, since that's commonly
black right now (left over from calibration/testing, not a real display
setting).

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


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--db', default=sweep_db.DEFAULT_DB_PATH)
    parser.add_argument('--mode', choices=['linear', 'gravity'], default='linear')
    parser.add_argument('--hz', type=float, default=1.0,
                         help="(--mode linear only) full top-to-bottom passes per second")
    parser.add_argument('--width', type=int, default=None,
                         help="band width in mm (default: 1/8 of the real z span)")
    parser.add_argument('--steps', type=int, default=15,
                         help="(--mode linear only) frames per pass - more is smoother motion but "
                              "needs a faster serial round-trip to hold the target Hz")
    parser.add_argument('--gravity', type=float, default=2000.0,
                         help="(--mode gravity only) acceleration in mm/s^2 - real gravity is 9800, "
                              "but that clears a real tree's height in well under a second; the "
                              "default gives a ~1.5s fall over a ~2.2m tree, visibly accelerating "
                              "without being over before it registers")
    parser.add_argument('--fps', type=float, default=12.0,
                         help="(--mode gravity only) target frame rate during the fall")
    parser.add_argument('--range-min', type=int, default=None)
    parser.add_argument('--range-max', type=int, default=None)
    parser.add_argument('--base-color', type=int, nargs=3, default=[255, 147, 41], metavar=('R', 'G', 'B'),
                         help="set once at startup (default: warm white/amber, like incandescent "
                              "string lights - a cozy backdrop for the bright random sweep band)")
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
    else:
        expected_fall_s = (2 * (range_max - range_min) / args.gravity) ** 0.5
        print(f"Display loop [gravity]: z range=[{range_min},{range_max}] (top-to-bottom) width={width} "
              f"gravity={args.gravity}mm/s^2 fps={args.fps} expected_fall~={expected_fall_s:.2f}s "
              f"- Ctrl+C to stop")

    pass_count = 0
    t_start = time.time()
    try:
        while True:
            color = (random.randint(0, 255), random.randint(0, 255), random.randint(0, 255))
            pass_count += 1
            t0 = time.time()
            if args.mode == 'linear':
                run_linear_pass(color, range_min, range_max, width, step, delay)
            else:
                run_gravity_pass(color, range_min, range_max, width, args.gravity, args.fps)
            pass_time = time.time() - t0
            print(f"  pass {pass_count}: color=rgb{color} actual={pass_time:.2f}s "
                  f"(avg over run={(time.time()-t_start)/pass_count:.2f}s)")
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        neoser.cleanup_serial()


if __name__ == "__main__":
    main()
