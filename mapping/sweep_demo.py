"""
Animates a SET_VOLUME_CYLINDRICAL/SET_VOLUME_CARTESIAN sweep: a bounding
window of fixed width moves across one axis (z, radius, omega, x, or y),
repainting each frame with clear_outside_volume=True so only the current
window is lit - a moving flat layer (z, x, or y), rotating pie slice
(omega), or expanding/contracting ring (radius).

Real mapped coordinates (mm for z/x/y, degrees for omega/radius... no,
radius is mm too) need --range-min/--range-max explicitly - there's no
synthetic-axis-sweep fallback anymore now that real data exists; see
full_sweep_sequence.py for the standard multi-axis sequence run against
whatever's currently loaded on the tree.

Usage (venv active):
    python3 sweep_demo.py --axis z --range-min -150 --range-max 2050 \\
        --width 80 --step 30 --delay 0.233
    python3 sweep_demo.py --axis omega --range-min 0 --range-max 360 \\
        --width 30 --step 15 --delay 0.233 --direction both
Ctrl+C to stop.
"""
import argparse
import time

import neotree_serial as neoser

CARTESIAN_AXES = {'x', 'y'}
CYLINDRICAL_AXES = {'z', 'radius', 'omega'}
INT16_MIN, INT16_MAX = -32768, 32767
UINT16_MAX = 65535


def send_volume_frame(axis, lo, hi, r, g, b):
    if axis == 'x':
        neoser.write_tree_set_volume_cartesian(
            neoser.ser, lo, hi, INT16_MIN, INT16_MAX, INT16_MIN, INT16_MAX, r, g, b, True)
    elif axis == 'y':
        neoser.write_tree_set_volume_cartesian(
            neoser.ser, INT16_MIN, INT16_MAX, lo, hi, INT16_MIN, INT16_MAX, r, g, b, True)
    elif axis == 'z':
        neoser.write_tree_set_volume_cylindrical(
            neoser.ser, lo, hi, 0, UINT16_MAX, 0, UINT16_MAX, r, g, b, True, verbose=False)
    elif axis == 'radius':
        neoser.write_tree_set_volume_cylindrical(
            neoser.ser, INT16_MIN, INT16_MAX, lo, hi, 0, UINT16_MAX, r, g, b, True, verbose=False)
    elif axis == 'omega':
        neoser.write_tree_set_volume_cylindrical(
            neoser.ser, INT16_MIN, INT16_MAX, 0, UINT16_MAX, lo, hi, r, g, b, True, verbose=False)
    else:
        raise ValueError(f"unknown axis: {axis}")


def run_sweep(axis, range_min, range_max, width, step, delay, color, loops=1,
              direction='forward', quiet=False):
    """
    Runs one axis sweep. direction: 'forward' (range_min->range_max),
    'reverse' (range_max->range_min), or 'both' (forward then reverse,
    per loop) - "each sweep should be able to be reversed" is standard
    practice for the full sequence, see full_sweep_sequence.py.
    """
    r, g, b = color
    directions = {'forward': ['forward'], 'reverse': ['reverse'], 'both': ['forward', 'reverse']}[direction]

    frame_count = 0
    t0 = time.time()
    for loop in range(loops):
        for d in directions:
            positions = range(range_min, range_max, step) if d == 'forward' else \
                range(range_max - width, range_min - step, -step)
            for pos in positions:
                lo = max(range_min, min(pos, range_max - width))
                hi = min(range_max, lo + width)
                send_volume_frame(axis, lo, hi, r, g, b)
                neoser.ser.reset_input_buffer()
                frame_count += 1
                time.sleep(delay)
            if not quiet:
                elapsed = time.time() - t0
                print(f"  axis={axis} loop {loop + 1}/{loops} [{d}] done "
                      f"({frame_count} frames, {frame_count / elapsed:.1f} fps avg)")
    return frame_count


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--axis', required=True, choices=['z', 'radius', 'omega', 'x', 'y'])
    parser.add_argument('--range-min', type=int, required=True)
    parser.add_argument('--range-max', type=int, required=True)
    parser.add_argument('--width', type=int, default=80, help="window width along the axis")
    parser.add_argument('--step', type=int, default=30, help="how far the window moves per frame")
    parser.add_argument('--delay', type=float, default=0.233,
                         help="seconds between frames (default is baseline speed - 50%% faster "
                              "than the prior 0.35s default)")
    parser.add_argument('--color', type=int, nargs=3, default=[255, 255, 255], metavar=('R', 'G', 'B'))
    parser.add_argument('--loops', type=int, default=1, help="how many full passes to run")
    parser.add_argument('--direction', choices=['forward', 'reverse', 'both'], default='forward')
    args = parser.parse_args()

    error = neoser.open_neotree_serial('/dev/ttyACM0', baudrate=115200, timeout=0.2)
    if error:
        print(error)
        return
    time.sleep(0.3)
    neoser.ser.read(neoser.ser.in_waiting or 1)

    print(f"Sweeping axis='{args.axis}' range=[{args.range_min},{args.range_max}] width={args.width} "
          f"step={args.step} delay={args.delay}s direction={args.direction} x{args.loops} loop(s) "
          f"- Ctrl+C to stop")
    try:
        run_sweep(args.axis, args.range_min, args.range_max, args.width, args.step,
                  args.delay, args.color, loops=args.loops, direction=args.direction)
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        neoser.cleanup_serial()


if __name__ == "__main__":
    main()
