"""
End-to-end check of the phased mapping driver (map_phases.py) on synthetic
data, through the real file and database plumbing: a made-up tree and rig
(test_bundle_synthetic.py) are written out as calibration files (intrinsics
per serial and focus, stereo per setup) and sweeps (pylon placements,
camera records, raw observations) in a scratch database, then phase 1 and
phase 2 run exactly as on real sweeps, and the results are compared with
the truth.

Run on the Pi (venv active, from mapping/): python3 test_map_phases_synthetic.py
"""
import json
import os
import shutil
import sys
import tempfile

import numpy as np

import bundle
import map_phases
import sweep_db
import test_bundle_synthetic as syn

SERIALS = {"A": ("SYN_A_TOP", "SYN_A_BOT"), "B": ("SYN_B_TOP", "SYN_B_BOT")}


def write_calibration(calib_dir, rigs, setup, focus):
    """Board-calibration files for the synthetic rigs (what calibrate_*.py would write)."""
    os.makedirs(os.path.join(calib_dir, setup), exist_ok=True)
    for name, rig in rigs.items():
        top, bottom = SERIALS[name]
        for serial, K, D in ((bottom, rig.K_bottom, rig.D_bottom), (top, rig.K_top, rig.D_top)):
            with open(os.path.join(calib_dir, f"intrinsics_{serial}_f{focus}.json"), "w") as f:
                json.dump({"serial": serial, "image_width": 1280, "image_height": 720, "focus": focus,
                           "camera_matrix": K.tolist(), "dist_coeffs": list(D)}, f)
        with open(os.path.join(calib_dir, setup, f"stereo_{name}.json"), "w") as f:
            json.dump({"pylon": name, "top_serial": top, "bottom_serial": bottom, "focus": focus, "setup": setup,
                       "image_width": 1280, "image_height": 720, "rotation_matrix": rig.R_rel.tolist(),
                       "translation_mm": list(rig.T_rel), "rms_reprojection_error_px": 0.3}, f)


def write_sweeps(conn, obs, station_rigs, setup, focus, labels):
    """Stations in pairs (A, B) per sweep, as the plan runs them."""
    sweeps = []
    for pair in range(0, len(station_rigs), 2):
        sweep = sweep_db.start_sweep(conn, notes=f"synthetic {setup}")
        sweeps.append(sweep)
        for s in (pair, pair + 1):
            pylon = station_rigs[s]
            top, bottom = SERIALS[pylon]
            sweep_db.record_pylon_placement(conn, sweep, pylon, 0, 2, 1280, 720, 592.0, f"charuco:{setup}")
            sweep_db.record_sweep_cameras(conn, sweep, pylon, top, bottom, focus, setup, labels[s])
            m = obs.station == s
            with conn:
                conn.executemany(
                    "INSERT INTO raw_observations (sweep_id, pylon_id, camera_position, led_position, found, "
                    "pixel_x, pixel_y, blob_count, blob_area, attempts, captured_at) VALUES (?, ?, ?, ?, 1, ?, ?, 1, 30, 1, ?)",
                    [(sweep, pylon, "top" if c else "bottom", int(l), float(u), float(v), sweep_db.now_iso())
                     for c, l, (u, v) in zip(obs.cam[m], obs.led[m], obs.uv[m])])
    return sweeps


def main():
    work = tempfile.mkdtemp(prefix="map_phases_")
    try:
        calib_dir = os.path.join(work, "calibration")
        conn = sweep_db.connect(os.path.join(work, "sweeps.sqlite3"))
        leds = syn.make_tree()
        top = set(np.nonzero(leds[:, 2] >= 1200)[0].tolist())
        stations = [0, 180, 60, 240, 120, 300]
        labels = ["S1", "S4", "S2", "S5", "S3", "S6"]

        cams1 = {n: (syn.true_camera(), syn.true_camera()) for n in "AB"}
        rigs1, st1, obs1, _t = syn.build_phase(leds, 1100, 1304, 1600, 592, cams1, stations)
        write_calibration(calib_dir, rigs1, "phase1", 60)
        sweeps1 = write_sweeps(conn, obs1, st1, "phase1", 60, labels)

        cams2 = {n: (syn.true_camera(), syn.true_camera()) for n in "AB"}
        rigs2, st2, obs2, _t = syn.build_phase(leds, 2300, 644, 940, 592, cams2, stations)
        write_calibration(calib_dir, rigs2, "phase2", 30)
        sweeps2 = write_sweeps(conn, obs2, st2, "phase2", 30, labels)

        run1, res1 = map_phases.run_phase(conn, "phase1", sweeps1, calib_dir)
        e1, (R1, t1) = syn.errors_after_rigid(res1.points, leds, top)
        syn.summary("TRUTH phase 1, top section", e1)

        anchor_ids = [l for (l,) in conn.execute("SELECT led_position FROM bundle_points WHERE run_id = ? AND anchor = 1",
                                                 (run1,))]
        ea = np.array([np.linalg.norm(R1 @ res1.points[l] + t1 - leds[l]) for l in anchor_ids])
        print(f"  TRUTH anchors: {len(ea)}, true error median {np.median(ea):.1f} mm, 99% {np.percentile(ea, 99):.1f} mm, "
              f"max {ea.max():.1f} mm; {sum(1 for l in anchor_ids if l not in top)} below the top section")
        run2, res2 = map_phases.run_phase(conn, "phase2", sweeps2, calib_dir, anchors_from=run1)
        e2 = np.array([np.linalg.norm(R1 @ res2.points[l] + t1 - leds[l]) for l in res2.points])
        syn.summary("TRUTH phase 2 anchored, whole tree (phase 1's frame)", e2)
        ok = np.median(e1) < 1.5 and np.median(e2) < 2.0
        print("\nPASS" if ok else "\nFAIL")
        return 0 if ok else 1
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
