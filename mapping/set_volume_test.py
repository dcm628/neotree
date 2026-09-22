"""
Sends one SET_VOLUME_CARTESIAN or SET_VOLUME_CYLINDRICAL command and prints
the firmware's response (which includes how many LEDs it found inside the
volume) - a quick way to test bounding-box logic against a synthetic
per-axis config written by write_axis_sweep_config.py, without needing
real mapped coordinates yet.

Examples (venv active):
    # with write_axis_sweep_config.py --axis z already written:
    python3 set_volume_test.py cylindrical --z-min 300 --z-max 400 --color 255 0 0 --clear

    # cartesian, with --axis radius (omega=0) already written:
    python3 set_volume_test.py cartesian --x-min 200 --x-max 300 --color 0 255 0 --clear
"""
import argparse
import time

import neotree_serial as neoser


def read_response(wait_s=1.5):
    end = time.time() + wait_s
    buf = b''
    while time.time() < end:
        chunk = neoser.ser.read(256)
        if chunk:
            buf += chunk
    return buf.decode(errors='replace')


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('mode', choices=['cartesian', 'cylindrical'])
    parser.add_argument('--color', type=int, nargs=3, default=[255, 255, 255], metavar=('R', 'G', 'B'))
    parser.add_argument('--clear', action='store_true',
                         help="clear LEDs outside the volume (default: leave them alone)")
    # cartesian bounds
    parser.add_argument('--x-min', type=int, default=-32768)
    parser.add_argument('--x-max', type=int, default=32767)
    parser.add_argument('--y-min', type=int, default=-32768)
    parser.add_argument('--y-max', type=int, default=32767)
    # cylindrical bounds (z shared with cartesian's z args below)
    parser.add_argument('--z-min', type=int, default=-32768)
    parser.add_argument('--z-max', type=int, default=32767)
    parser.add_argument('--radius-min', type=int, default=0)
    parser.add_argument('--radius-max', type=int, default=65535)
    parser.add_argument('--omega-min', type=int, default=0)
    parser.add_argument('--omega-max', type=int, default=65535)
    args = parser.parse_args()

    error = neoser.open_neotree_serial('/dev/ttyACM0', baudrate=115200)
    if error:
        print(error)
        return
    time.sleep(0.3)
    neoser.ser.read(neoser.ser.in_waiting or 1)  # drop any backlog
    r, g, b = args.color

    if args.mode == 'cartesian':
        neoser.write_tree_set_volume_cartesian(
            neoser.ser, args.x_min, args.x_max, args.y_min, args.y_max,
            args.z_min, args.z_max, r, g, b, args.clear)
    else:
        neoser.write_tree_set_volume_cylindrical(
            neoser.ser, args.z_min, args.z_max, args.radius_min, args.radius_max,
            args.omega_min, args.omega_max, r, g, b, args.clear)

    print(read_response())
    neoser.cleanup_serial()


if __name__ == "__main__":
    main()
