"""
Animates a SET_VOLUME_CYLINDRICAL sweep: a bounding window of fixed width
moves across one axis (z, radius, or omega) over the full 0..999 synthetic
range, repainting each frame with clear_outside_volume=True so only the
current window is lit - a moving flat layer (z), rotating pie slice
(omega), or expanding/contracting ring (radius).

Requires the matching synthetic config already written via
write_axis_sweep_config.py --axis <same axis>, and a CONFIG_RELOAD sent
since (write_axis_sweep_config.py does this automatically at the end).

Usage (venv active):
    python3 sweep_demo.py --axis omega --width 30 --step 10 --delay 0.1
    python3 sweep_demo.py --axis z --width 50 --step 15 --delay 0.15 --loops 3
Ctrl+C to stop.
"""
import argparse
import time

import neotree_serial as neoser

AXIS_RANGE = 1000  # matches write_axis_sweep_config.py's synthetic 0..999 sweep


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--axis', required=True, choices=['z', 'radius', 'omega'])
    parser.add_argument('--range-min', type=int, default=None,
                         help="override the swept range's start (default: 0, the synthetic-axis-sweep "
                              "convention). Use real min/max mm (or degrees for omega) for real mapped data.")
    parser.add_argument('--range-max', type=int, default=None,
                         help="override the swept range's end (default: AXIS_RANGE, i.e. 999)")
    parser.add_argument('--width', type=int, default=30, help="window width along the axis")
    parser.add_argument('--step', type=int, default=10, help="how far the window moves per frame")
    parser.add_argument('--delay', type=float, default=0.1, help="seconds between frames")
    parser.add_argument('--color', type=int, nargs=3, default=[255, 255, 255], metavar=('R', 'G', 'B'))
    parser.add_argument('--loops', type=int, default=1, help="how many full passes to run")
    args = parser.parse_args()

    error = neoser.open_neotree_serial('/dev/ttyACM0', baudrate=115200, timeout=0.2)
    if error:
        print(error)
        return
    time.sleep(0.3)
    neoser.ser.read(neoser.ser.in_waiting or 1)
    r, g, b = args.color
    range_min = 0 if args.range_min is None else args.range_min
    range_max = AXIS_RANGE if args.range_max is None else args.range_max

    print(f"Sweeping axis='{args.axis}' range=[{range_min},{range_max}] width={args.width} "
          f"step={args.step} delay={args.delay}s x{args.loops} loop(s) - Ctrl+C to stop")
    frame_count = 0
    t0 = time.time()
    try:
        for loop in range(args.loops):
            pos = range_min
            while pos < range_max:
                lo, hi = pos, min(pos + args.width, range_max - 1)
                if args.axis == 'z':
                    neoser.write_tree_set_volume_cylindrical(
                        neoser.ser, lo, hi, 0, 65535, 0, 65535, r, g, b, True, verbose=False)
                elif args.axis == 'radius':
                    neoser.write_tree_set_volume_cylindrical(
                        neoser.ser, -32768, 32767, lo, hi, 0, 65535, r, g, b, True, verbose=False)
                else:  # omega
                    neoser.write_tree_set_volume_cylindrical(
                        neoser.ser, -32768, 32767, 0, 65535, lo, hi, r, g, b, True, verbose=False)
                neoser.ser.reset_input_buffer()
                pos += args.step
                frame_count += 1
                time.sleep(args.delay)
            elapsed = time.time() - t0
            print(f"  loop {loop + 1}/{args.loops} done "
                  f"({frame_count} frames, {frame_count / elapsed:.0f} fps avg)")
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        neoser.cleanup_serial()


if __name__ == "__main__":
    main()
