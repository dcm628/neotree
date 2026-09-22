"""
Runs the standard 4-sweep visual validation sequence against whatever's
currently loaded on the tree (RESET_POS_CONFIG_TO_DEFAULT the compiled-in
config first if that's not already current):

    1. z      - bottom to top, then reversed
    2. x      - front to back, then reversed
    3. y      - front to back, then reversed
    4. omega  - 0 to 360 (pie slice), then reversed

Each sweep is independently parameterized (width/step/delay) and each
runs forward-then-reverse by default ("every sweep should be able to be
reversed"). Real coordinate ranges are pulled from global_estimates
(same cov_xx+cov_yy+cov_zz outlier filter as generate_pos_config_header.py
--source global) unless overridden, so this always matches whatever's
actually mapped rather than a hardcoded range.

Baseline speed is delay=0.233s - 50% faster than the 0.35s used through
the first merged-dataset validation sweep (step/width unchanged, so this
is purely a frame-rate increase: ~step/delay mm-or-deg per second, up
1.5x). Override per-sweep with --z-delay/--x-delay/--y-delay/--omega-delay
(and the matching --*-width/--*-step) if one axis needs a different pace.

Usage (venv active):
    python3 full_sweep_sequence.py
    python3 full_sweep_sequence.py --sweeps z omega
    python3 full_sweep_sequence.py --omega-delay 0.4 --omega-width 45
"""
import argparse
import time

import sweep_db
import sweep_demo
import neotree_serial as neoser

BASELINE_DELAY = 0.35 / 1.5  # 50% faster than the prior 0.35s baseline
RANGE_PADDING_MM = 50  # small margin beyond the observed min/max so edge LEDs aren't clipped

SWEEP_ORDER = ['z', 'x', 'y', 'omega']


def real_ranges(db_path, max_cov_trace=1000.0):
    conn = sweep_db.connect(db_path)
    rows = list(conn.execute(
        "SELECT x_mm, y_mm, z_mm FROM global_estimates WHERE cov_xx+cov_yy+cov_zz <= ?",
        (max_cov_trace,)))
    conn.close()
    if not rows:
        raise SystemExit("no usable global_estimates - run a capture sweep + align_sweep.py first")
    xs = [r[0] for r in rows]
    ys = [r[1] for r in rows]
    zs = [r[2] for r in rows]
    return {
        'x': (int(min(xs)) - RANGE_PADDING_MM, int(max(xs)) + RANGE_PADDING_MM),
        'y': (int(min(ys)) - RANGE_PADDING_MM, int(max(ys)) + RANGE_PADDING_MM),
        'z': (int(min(zs)) - RANGE_PADDING_MM, int(max(zs)) + RANGE_PADDING_MM),
        'omega': (0, 360),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--db', default=sweep_db.DEFAULT_DB_PATH)
    parser.add_argument('--sweeps', nargs='+', choices=SWEEP_ORDER, default=SWEEP_ORDER,
                         help="which sweeps to run, in order (default: all 4)")
    parser.add_argument('--color', type=int, nargs=3, default=[255, 255, 255], metavar=('R', 'G', 'B'))
    parser.add_argument('--loops', type=int, default=1, help="forward+reverse pairs per sweep")

    for axis, default_width, default_step in [('z', 80, 30), ('x', 80, 30), ('y', 80, 30), ('omega', 30, 15)]:
        parser.add_argument(f'--{axis}-width', type=int, default=default_width)
        parser.add_argument(f'--{axis}-step', type=int, default=default_step)
        parser.add_argument(f'--{axis}-delay', type=float, default=BASELINE_DELAY)
        parser.add_argument(f'--{axis}-range-min', type=int, default=None)
        parser.add_argument(f'--{axis}-range-max', type=int, default=None)

    args = parser.parse_args()

    ranges = real_ranges(args.db)

    error = neoser.open_neotree_serial('/dev/ttyACM0', baudrate=115200, timeout=0.2)
    if error:
        print(error)
        return
    time.sleep(0.3)
    neoser.ser.read(neoser.ser.in_waiting or 1)

    try:
        for axis in args.sweeps:
            range_min = getattr(args, f'{axis}_range_min')
            range_max = getattr(args, f'{axis}_range_max')
            if range_min is None:
                range_min = ranges[axis][0]
            if range_max is None:
                range_max = ranges[axis][1]
            width = getattr(args, f'{axis}_width')
            step = getattr(args, f'{axis}_step')
            delay = getattr(args, f'{axis}_delay')

            print(f"\n=== sweep: {axis} range=[{range_min},{range_max}] width={width} "
                  f"step={step} delay={delay:.3f}s (forward + reverse) ===")
            sweep_demo.run_sweep(axis, range_min, range_max, width, step, delay,
                                  args.color, loops=args.loops, direction='both')
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        neoser.cleanup_serial()


if __name__ == "__main__":
    main()
