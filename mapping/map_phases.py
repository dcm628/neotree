"""
Phased mapping (docs/CALIBRATION.md): turns several sweeps' raw pixel
observations into LED positions with one bundle adjustment (bundle.py).

phase1 - the top section from close range: every station's pose and every
         LED solved together, each pylon's stereo angle and each camera's
         intrinsics refined near their board calibration. The best LEDs are
         marked as anchors (bundle.choose_anchors: seen from two stations at
         a real angle, fitting every sighting, precise, and within the
         wiring limits of their neighbours).
phase2 - the whole tree from further back, anchored: the anchors (from a
         phase-1 run) fix the frame and each station's pose.
show   - a run's summary.

Each sweep must have been captured with calibrated cameras
(capture_sweep.py --focus F --setup S --station-a ...): its pylons'
camera serials, focus and setup pick the calibration. The wiring limits
(neighbours at most 100 mm apart, 300 mm at strand joints) go in as soft
constraints and are checked afterwards.

Results go to the bundle_runs / bundle_points tables. The frame is phase
1's first station's bottom camera (OpenCV: x right, y down, z forward);
turning it into the tree's frame (trunk up) is a later step.

Usage (on the Pi, venv active, from mapping/):
    python3 map_phases.py phase1 --sweeps 21 22 23
    python3 map_phases.py phase2 --sweeps 24 25 26 --anchors-from 1
    python3 map_phases.py show 2
"""
import argparse
import json

import numpy as np

import bundle
import pylon_geometry as geom
import sweep_db


def load_rig(name, top_serial, bottom_serial, focus, setup, calib_dir):
    ib = geom.load_intrinsics(bottom_serial, calib_dir, focus)
    it = geom.load_intrinsics(top_serial, calib_dir, focus)
    st = geom.find_stereo(bottom_serial, top_serial, calib_dir, setup)
    missing = [what for what, d in (("bottom intrinsics", ib), ("top intrinsics", it), ("stereo", st)) if d is None]
    if missing:
        raise SystemExit(f"rig {name}: no {', '.join(missing)} for cameras {bottom_serial}/{top_serial} "
                         f"at focus {focus}, setup {setup}")
    return bundle.Rig(name, np.array(ib["camera_matrix"]), np.array(ib["dist_coeffs"]),
                      np.array(it["camera_matrix"]), np.array(it["dist_coeffs"]),
                      np.array(st["rotation_matrix"]), np.array(st["translation_mm"]))


def load_sweeps(conn, sweeps, calib_dir):
    """Rigs, the rig of each station, observations and station labels for these sweeps."""
    rigs, station_rigs, labels, rows = {}, [], [], []
    for sweep in sweeps:
        pylons = conn.execute("SELECT pylon_id FROM pylon_placements WHERE sweep_id = ? ORDER BY pylon_id",
                              (sweep,)).fetchall()
        if not pylons:
            raise SystemExit(f"sweep {sweep}: not found")
        for (pylon,) in pylons:
            cams = sweep_db.get_sweep_cameras(conn, sweep, pylon)
            if cams is None:
                raise SystemExit(f"sweep {sweep} pylon {pylon}: no camera record - captured before calibrated "
                                 f"sweeps (capture_sweep.py --focus/--setup)")
            top_serial, bottom_serial, focus, setup, station = cams
            name = f"{setup}:{pylon}:{bottom_serial}/{top_serial}@{focus}"
            if name not in rigs:
                rigs[name] = load_rig(name, top_serial, bottom_serial, focus, setup, calib_dir)
            s = len(station_rigs)
            station_rigs.append(name)
            labels.append(f"sweep {sweep} pylon {pylon}" + (f" at {station}" if station else ""))
            for led, pos, u, v in conn.execute(
                    "SELECT led_position, camera_position, pixel_x, pixel_y FROM raw_observations "
                    "WHERE sweep_id = ? AND pylon_id = ? AND found = 1", (sweep, pylon)):
                rows.append((s, 0 if pos == "bottom" else 1, led, u, v))
    return rigs, station_rigs, labels, bundle.Observations.from_rows(rows)


def run_phase(conn, phase, sweeps, calib_dir=geom.DEFAULT_CALIBRATION_DIR, anchors_from=None,
              anchor_sigma_mm=3.0, notes=None, quiet=False):
    say = (lambda *a: None) if quiet else print
    rigs, station_rigs, labels, obs = load_sweeps(conn, sweeps, calib_dir)
    say(f"{phase}: {len(obs.led)} sightings of {len(set(obs.led.tolist()))} LEDs from {len(station_rigs)} stations, "
        f"{len(rigs)} rig setup(s)")
    anchors = None
    if anchors_from is not None:
        anchors = {led: (np.array([x, y, z]), max(sig, 0.5)) for led, x, y, z, sig in conn.execute(
            "SELECT led_position, x_mm, y_mm, z_mm, sigma_mm FROM bundle_points WHERE run_id = ? AND anchor = 1",
            (anchors_from,))}
        if not anchors:
            raise SystemExit(f"run {anchors_from} has no anchors")
        say(f"  {len(anchors)} anchors from run {anchors_from}")
    wiring = bundle.wiring_limits(set(obs.led.tolist()) | set(anchors or {}))
    b = bundle.Bundle(rigs, station_rigs, obs, refine_rigs=True, refine_intrinsics=True,
                      anchors=anchors, wiring=wiring)
    res = b.solve()
    if b.single_sighting:
        say(f"  {len(b.single_sighting)} LEDs seen by only one camera can't be placed - left out")

    sigma = {l: float(np.sqrt(np.trace(res.cov[l]))) for l in res.points}
    chosen = bundle.choose_anchors(res, wiring, max_sigma_mm=anchor_sigma_mm)
    is_anchor = {l: l in chosen for l in res.points}
    with conn:
        cur = conn.execute(
            "INSERT INTO bundle_runs (created_at, phase, sweeps, anchors_from, rms_px, dropped, details_json, notes) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
            (sweep_db.now_iso(), phase, json.dumps(list(sweeps)), anchors_from, res.rms_px, res.dropped,
             json.dumps({"stations": labels,
                         "rig_delta_mrad": {k: (np.asarray(v) * 1000).tolist() for k, v in res.rig_delta.items()},
                         "intrinsics": {f"{k[0]}|{'top' if k[1] else 'bottom'}": v.tolist()
                                        for k, v in res.intrinsics.items()}}), notes))
        run_id = cur.lastrowid
        conn.executemany(
            "INSERT INTO bundle_points (run_id, led_position, x_mm, y_mm, z_mm, sigma_mm, n_stations, n_obs, anchor) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
            [(run_id, int(l), *map(float, res.points[l]), sigma[l] if np.isfinite(sigma[l]) else 1e6,
              res.n_stations[l], res.n_obs[l], int(is_anchor[l])) for l in res.points])
    if not quiet:
        summarize(conn, run_id, b, res, anchors)
    return run_id, res


def summarize(conn, run_id, b=None, res=None, anchors=None):
    run = conn.execute("SELECT phase, sweeps, anchors_from, rms_px, dropped, details_json FROM bundle_runs "
                       "WHERE run_id = ?", (run_id,)).fetchone()
    if run is None:
        raise SystemExit(f"no run {run_id}")
    phase, sweeps, anchors_from, rms, dropped, details = run
    details = json.loads(details)
    pts = conn.execute("SELECT led_position, x_mm, y_mm, z_mm, sigma_mm, n_stations, anchor FROM bundle_points "
                       "WHERE run_id = ?", (run_id,)).fetchall()
    sig = np.array([p[4] for p in pts])
    nst = np.array([p[5] for p in pts])
    print(f"\nrun {run_id} ({phase}, sweeps {json.loads(sweeps)}): {len(pts)} LEDs, "
          f"reprojection RMS {rms:.2f} px, {dropped} sightings rejected as bad blobs")
    print(f"  seen by 1 station: {(nst == 1).sum()}, 2: {(nst == 2).sum()}, 3+: {(nst >= 3).sum()}")
    fin = sig[sig < 1e5]
    if len(fin):
        print(f"  estimated accuracy: median {np.median(fin):.1f} mm, 90% {np.percentile(fin, 90):.1f} mm")
    print(f"  anchors marked: {sum(p[6] for p in pts)}")
    for k, v in details["rig_delta_mrad"].items():
        print(f"  rig {k}: stereo angle corrected by {np.linalg.norm(v):.2f} mrad")
    pos = {p[0]: np.array(p[1:4]) for p in pts}
    limits = bundle.wiring_limits(pos)
    broken = [(i, j, np.linalg.norm(pos[i] - pos[j]), L) for i, j, L in limits if np.linalg.norm(pos[i] - pos[j]) > L + 10]
    print(f"  wiring: {len(broken)} of {len(limits)} neighbour pairs more than 10 mm over the limit"
          + (": " + ", ".join(f"{i}-{j} {d:.0f}mm" for i, j, d, _L in broken[:8]) if broken else ""))
    if anchors_from is not None:
        a = {l: np.array([x, y, z]) for l, x, y, z in conn.execute(
            "SELECT led_position, x_mm, y_mm, z_mm FROM bundle_points WHERE run_id = ? AND anchor = 1", (anchors_from,))}
        d = np.array([np.linalg.norm(pos[l] - a[l]) for l in a if l in pos])
        if len(d):
            print(f"  anchors re-solved here vs phase 1: median {np.median(d):.1f} mm, max {d.max():.1f} mm")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)
    for name in ("phase1", "phase2"):
        p = sub.add_parser(name)
        p.add_argument('--sweeps', type=int, nargs='+', required=True)
        p.add_argument('--db', default=sweep_db.DEFAULT_DB_PATH)
        p.add_argument('--calib-dir', default=geom.DEFAULT_CALIBRATION_DIR)
        p.add_argument('--anchor-sigma', type=float, default=3.0, help="mark as anchors LEDs estimated within this (mm)")
        p.add_argument('--notes', default=None)
    sub.choices["phase2"].add_argument('--anchors-from', type=int, required=True, help="the phase 1 run")
    s = sub.add_parser("show")
    s.add_argument('run_id', type=int)
    s.add_argument('--db', default=sweep_db.DEFAULT_DB_PATH)
    args = parser.parse_args()
    conn = sweep_db.connect(args.db)
    if args.cmd == "show":
        summarize(conn, args.run_id)
    else:
        run_phase(conn, args.cmd, args.sweeps, args.calib_dir,
                  anchors_from=getattr(args, "anchors_from", None), anchor_sigma_mm=args.anchor_sigma,
                  notes=args.notes)


if __name__ == "__main__":
    main()
