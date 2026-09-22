"""
Re-runs triangulation for an existing sweep's raw_observations against the
current pylon_geometry code, overwriting that sweep's session_solves.
Raw pixel data is untouched by this - it exists so a geometry-model fix
(wrong axis mapping, corrected camera spacing, etc.) can be applied to
already-collected data without a new physical capture sweep.

Uses the per-sweep, per-pylon top-camera tilt correction from
pylon_tilt_fits if one has been fit (see fit_camera_tilt.py - the
standard practice is to fit this fresh for every sweep/pylon, since the
rig is deliberately hand-aimed rather than kept parallel), falling back
to pylon_geometry's module-default rotation (fit for pylon A specifically
- not generally correct for a different pylon) with a warning if not.

Usage (venv active):
    python3 retriangulate_sweep.py --sweep-id 1 --pylon-id A --spacing-mm 592
"""
import argparse

import pylon_geometry as geom
import sweep_db


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--db', default=sweep_db.DEFAULT_DB_PATH)
    parser.add_argument('--sweep-id', default='latest', help="sweep_id to re-triangulate, or 'latest'")
    parser.add_argument('--pylon-id', default='A')
    parser.add_argument('--spacing-mm', type=float, default=geom.PYLON_CAMERA_SPACING_MM)
    parser.add_argument('--pixel-sigma', type=float, default=1.0)
    args = parser.parse_args()

    conn = sweep_db.connect(args.db)
    if args.sweep_id == 'latest':
        sweep_id = conn.execute("SELECT MAX(sweep_id) FROM sweeps").fetchone()[0]
    else:
        sweep_id = int(args.sweep_id)

    placement = conn.execute(
        "SELECT capture_width, capture_height FROM pylon_placements "
        "WHERE sweep_id = ? AND pylon_id = ?", (sweep_id, args.pylon_id)).fetchone()
    if placement is None:
        raise SystemExit(f"no pylon_placements row for sweep {sweep_id} pylon {args.pylon_id}")
    width, height = placement

    tilt_rvec = sweep_db.get_pylon_tilt_fit(conn, sweep_id, args.pylon_id)
    if tilt_rvec is not None:
        print(f"Using fitted tilt correction from pylon_tilt_fits for sweep {sweep_id} pylon {args.pylon_id}")
        bottom_model, top_model = geom.make_pylon_cameras(
            width, height, spacing_mm=args.spacing_mm, top_rotation_rvec=tilt_rvec)
    else:
        print(f"WARNING: no pylon_tilt_fits entry for sweep {sweep_id} pylon {args.pylon_id} - "
              f"run fit_camera_tilt.py first. Falling back to pylon_geometry's module-default "
              f"rotation (fit for pylon A specifically, likely wrong for a different pylon).")
        bottom_model, top_model = geom.make_pylon_cameras(width, height, spacing_mm=args.spacing_mm)

    observations = {}  # led_position -> {'top': (u,v), 'bottom': (u,v)}
    for led, cam_pos, u, v in conn.execute(
            "SELECT led_position, camera_position, pixel_x, pixel_y FROM raw_observations "
            "WHERE sweep_id = ? AND pylon_id = ? AND found = 1", (sweep_id, args.pylon_id)):
        observations.setdefault(led, {})[cam_pos] = (u, v)

    conn.execute("DELETE FROM session_solves WHERE sweep_id = ? AND pylon_id = ?",
                 (sweep_id, args.pylon_id))

    solved = 0
    for led, obs in observations.items():
        if 'top' not in obs or 'bottom' not in obs:
            continue
        result = geom.triangulate_pylon_observation(
            bottom_model, top_model, obs['bottom'], obs['top'], pixel_sigma_px=args.pixel_sigma)
        sweep_db.record_session_solve(conn, sweep_id, args.pylon_id, led, result)
        solved += 1
    conn.commit()
    conn.close()

    print(f"Re-triangulated sweep {sweep_id} pylon {args.pylon_id}: "
          f"{solved} LEDs solved (spacing={args.spacing_mm}mm, {width}x{height})")


if __name__ == "__main__":
    main()
