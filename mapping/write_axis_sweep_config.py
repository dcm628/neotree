"""
Writes a synthetic position config where ONE cylindrical axis (z, radius,
or omega) increases linearly with LED index (0..999) while the other two
are held at fixed constants. This has nothing to do with the tree's real
geometry - it exists purely to smoke-test SET_VOLUME_CARTESIAN/
SET_VOLUME_CYLINDRICAL's bounding-box logic before real camera-mapped
coordinates exist: with (say) z=led_index, a "z in [300,400]" volume
command should light up roughly LEDs 300-400 and nothing else, which is
easy to verify by eye or by reading positions back even without a real
3D mapping.

Only one axis is written per run - re-run with a different --axis to
overwrite the previous one, since there's a single position-config array,
not one saved per test scenario.

Usage (venv active):
    python3 write_axis_sweep_config.py --axis z
    python3 write_axis_sweep_config.py --axis omega
    python3 write_axis_sweep_config.py --axis radius
"""
import argparse
import time

import neotree_serial as neoser

TOTAL_LEDS = 1000
FIXED_Z = 500
FIXED_RADIUS = 500  # non-zero so omega sweeps actually move x/y
FIXED_OMEGA = 0      # 0 so radius sweeps land on a single simple ray (y=0)


def coords_for(axis, i):
    """(z, radius, omega) for LED index i, varying only `axis` linearly."""
    if axis == 'z':
        return i, FIXED_RADIUS, FIXED_OMEGA
    elif axis == 'radius':
        return FIXED_Z, i, FIXED_OMEGA
    elif axis == 'omega':
        return FIXED_Z, FIXED_RADIUS, i
    else:
        raise ValueError(f"unknown axis: {axis}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--axis', required=True, choices=['z', 'radius', 'omega'],
                         help="which cylindrical axis sweeps linearly with LED index")
    parser.add_argument('--count', type=int, default=TOTAL_LEDS,
                         help="how many LED positions to write (default: all 1000)")
    args = parser.parse_args()

    error = neoser.open_neotree_serial('/dev/ttyACM0', baudrate=115200)
    if error:
        print(error)
        return

    print(f"Writing axis='{args.axis}' sweep config for {args.count} LEDs "
          f"(fixed z={FIXED_Z} radius={FIXED_RADIUS} omega={FIXED_OMEGA} "
          f"except the swept axis)...")
    t0 = time.time()
    for i in range(args.count):
        z, radius, omega = coords_for(args.axis, i)
        neoser.write_tree_pos_cylindrical(neoser.ser, i, z, radius, omega)
        if (i + 1) % 100 == 0:
            print(f"  {i + 1}/{args.count} written, elapsed={time.time() - t0:.1f}s")

    print(f"Done in {time.time() - t0:.1f}s")
    neoser.cleanup_serial()


if __name__ == "__main__":
    main()
