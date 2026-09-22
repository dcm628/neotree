"""
Fun, display-only loop: a colored band sweeps top to bottom, continuously,
a fresh random color each pass, at a configurable rate (default 1Hz - one
full top-to-bottom pass per second). The band is the secondary-color
overlay, and clear_outside_volume lets the base color show through
everywhere outside it - so the base color is set once at startup
(--base-color) rather than left as whatever's currently loaded, since
that's commonly black right now (left over from calibration/testing, not
a real display setting).

Real z range is pulled from global_estimates automatically (same as
full_sweep_sequence.py), so it covers whatever's currently mapped.

Usage (venv active):
    python3 display_loop_sweep.py
    python3 display_loop_sweep.py --hz 0.5 --width 200 --base-color 0 60 20
"""
import argparse
import random
import time

import full_sweep_sequence
import sweep_db
import sweep_demo
import neotree_serial as neoser


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--db', default=sweep_db.DEFAULT_DB_PATH)
    parser.add_argument('--hz', type=float, default=1.0, help="full top-to-bottom passes per second")
    parser.add_argument('--width', type=int, default=None,
                         help="band width in mm (default: 1/8 of the real z span)")
    parser.add_argument('--steps', type=int, default=15, help="frames per pass - more is smoother "
                         "motion but needs a faster serial round-trip to hold the target Hz")
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
    print(f"Display loop: z range=[{range_min},{range_max}] (top-to-bottom) width={width} "
          f"step={step} target={args.hz}Hz/pass ({args.steps} frames, delay={delay:.3f}s) "
          f"- Ctrl+C to stop")
    pass_count = 0
    t_start = time.time()
    try:
        while True:
            color = (random.randint(0, 255), random.randint(0, 255), random.randint(0, 255))
            pass_count += 1
            t0 = time.time()
            for pos in range(range_max, range_min - step, -step):
                hi = max(range_min + width, min(pos, range_max))
                lo = hi - width
                sweep_demo.send_volume_frame('z', lo, hi, *color)
                neoser.ser.reset_input_buffer()
                time.sleep(delay)
            pass_time = time.time() - t0
            print(f"  pass {pass_count}: color=rgb{color} actual={pass_time:.2f}s "
                  f"(target={1.0/args.hz:.2f}s, avg over run={ (time.time()-t_start)/pass_count:.2f}s)")
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        neoser.cleanup_serial()


if __name__ == "__main__":
    main()
