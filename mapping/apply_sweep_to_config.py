"""
Writes real triangulated LED positions from one sweep_db.py session into
the firmware's position config: solved LEDs get their real pylon-local
(x, y, z) via LED_POS_UPDATE_CARTESIAN; every other LED in the 0..count-1
range gets pushed to the null sentinel position (see
pylon_geometry.NULL_Z_MM/NULL_RADIUS_MM/NULL_OMEGA_DEG - out of bounds on
every axis, not just z) via LED_POS_UPDATE_CYLINDRICAL, so a volume
command using any sane bound naturally excludes unsolved LEDs instead of
leaving them at stale/default coordinates from an earlier synthetic test
config.

This does NOT attempt any cross-pylon/cross-sweep alignment - it just
takes one session's solves as-is, in that pylon's own local frame. Good
enough for a single-pylon visual sanity check of the volume-bounding
logic against real (if partial, if uncalibrated) data; not a real map yet.

Usage (venv active):
    python3 apply_sweep_to_config.py --sweep-id 1 --pylon-id A
    python3 apply_sweep_to_config.py --sweep-id latest --pylon-id A --count 1000
"""
import argparse
import time

import neotree_serial as neoser
import pylon_geometry as geom
import sweep_db


def confirm_write(deadline_s=2.0):
    """Same pattern as write_axis_sweep_config.py: poll short reads until
    the firmware's per-write verify print shows up or the deadline passes."""
    deadline = time.time() + deadline_s
    buf = b''
    while time.time() < deadline:
        chunk = neoser.ser.read(256)
        if chunk:
            buf += chunk
            if b'verify OK' in buf or b'verify FAILED' in buf:
                break
    return buf


def resolve_sweep_id(conn, sweep_id_arg):
    if sweep_id_arg != 'latest':
        return int(sweep_id_arg)
    row = conn.execute("SELECT MAX(sweep_id) FROM sweeps").fetchone()
    if row is None or row[0] is None:
        raise SystemExit("no sweeps found in the database")
    return row[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--db', default=sweep_db.DEFAULT_DB_PATH)
    parser.add_argument('--sweep-id', default='latest', help="sweep_id to apply, or 'latest'")
    parser.add_argument('--pylon-id', default='A')
    parser.add_argument('--count', type=int, default=1000, help="total LED range, 0..count-1")
    parser.add_argument('--standoff-mm', type=float, default=0.0,
                         help="horizontal distance from the bottom camera to the trunk "
                              "(coarse pylon->tree frame shift, see pylon_geometry."
                              "pylon_to_coarse_tree_frame)")
    parser.add_argument('--ground-offset-mm', type=float, default=0.0,
                         help="height of the bottom camera above the tree's base")
    args = parser.parse_args()

    conn = sweep_db.connect(args.db)
    sweep_id = resolve_sweep_id(conn, args.sweep_id)

    solves = {}
    for led_position, x, y, z in conn.execute(
            "SELECT led_position, x_mm, y_mm, z_mm FROM session_solves "
            "WHERE sweep_id = ? AND pylon_id = ?", (sweep_id, args.pylon_id)):
        solves[led_position] = (x, y, z)
    conn.close()

    print(f"Applying sweep {sweep_id} pylon {args.pylon_id}: "
          f"{len(solves)} solved LEDs out of {args.count} total")
    if args.standoff_mm or args.ground_offset_mm:
        print(f"Applying coarse pylon->tree shift: standoff={args.standoff_mm}mm "
              f"ground_offset={args.ground_offset_mm}mm (see pylon_geometry."
              f"pylon_to_coarse_tree_frame - not real alignment, a rough placeholder)")

    error = neoser.open_neotree_serial('/dev/ttyACM0', baudrate=115200, timeout=0.2)
    if error:
        print(error)
        return

    t0 = time.time()
    failed = []
    solved_count = 0
    nulled_count = 0
    for led in range(args.count):
        neoser.ser.reset_input_buffer()
        if led in solves:
            x, y, z = geom.pylon_to_coarse_tree_frame(
                solves[led], args.standoff_mm, args.ground_offset_mm)
            neoser.write_tree_pos_cartesian(neoser.ser, led, int(round(x)), int(round(y)), int(round(z)))
            solved_count += 1
        else:
            neoser.write_tree_pos_cylindrical(
                neoser.ser, led, geom.NULL_Z_MM, geom.NULL_RADIUS_MM, geom.NULL_OMEGA_DEG)
            nulled_count += 1

        buf = confirm_write()
        if b'verify OK' not in buf:
            failed.append(led)
            print(f"  WARNING: no verify OK for LED {led} - response was: {buf!r}")

        if (led + 1) % 100 == 0:
            print(f"  {led + 1}/{args.count} written, elapsed={time.time() - t0:.1f}s")

    print(f"Done in {time.time() - t0:.1f}s: {solved_count} real positions, "
          f"{nulled_count} nulled out")
    if failed:
        print(f"WARNING: {len(failed)} LEDs did not confirm: {failed}")

    print("Sending CONFIG_RELOAD to sync live LED objects...")
    neoser.ser.reset_input_buffer()
    neoser.write_tree_config_reload(neoser.ser)
    time.sleep(1.0)
    neoser.ser.read(neoser.ser.in_waiting or 1)
    print("Done.")

    neoser.cleanup_serial()


if __name__ == "__main__":
    main()
