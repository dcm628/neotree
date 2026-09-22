"""
Fits a small rotation correction for the top camera (relative to the
parallel-cameras assumption in pylon_geometry.py) by minimizing total ray
residual across a sweep's real LED point correspondences - self-
calibration using the LEDs themselves as calibration targets, since we
don't have a checkerboard calibration set up.

Coarse-to-fine grid search over a 3-parameter rotation vector (Rodrigues
form, via cv2.Rodrigues) - no scipy available on the Pi, and 3 parameters
over a small smooth objective doesn't need gradients.

Usage (venv active):
    python3 fit_camera_tilt.py --sweep-id 2 --pylon-id A
"""
import argparse

import cv2
import numpy as np

import pylon_geometry as geom
import sweep_db


def total_residual(rvec, cameras_pixels, bottom_model, top_origin, top_focal, top_principal):
    R, _ = cv2.Rodrigues(np.array(rvec, dtype=float))
    total = 0.0
    for bu, bv, tu, tv in cameras_pixels:
        d_bottom = bottom_model.ray_direction(bu, bv)
        # top camera's "ideal" (parallel-assumption) ray direction, then rotated
        right = (tv - top_principal[1]) / top_focal
        up = (tu - top_principal[0]) / top_focal
        d_top_ideal = np.array([right, 1.0, up])
        d_top_ideal = d_top_ideal / np.linalg.norm(d_top_ideal)
        d_top = R @ d_top_ideal

        origins = [bottom_model.origin, top_origin]
        directions = [d_bottom, d_top]
        A = np.zeros((3, 3)); b = np.zeros(3)
        for o, d in zip(origins, directions):
            M = np.eye(3) - np.outer(d, d)
            A += M; b += M @ o
        pt, *_ = np.linalg.lstsq(A, b, rcond=None)
        for o, d in zip(origins, directions):
            v = pt - o
            perp = v - np.dot(v, d) * d
            total += np.dot(perp, perp)
    return total


def grid_search(cameras_pixels, bottom_model, top_origin, top_focal, top_principal,
                 center, half_range, steps, depth):
    best_rvec = center
    best_score = total_residual(center, cameras_pixels, bottom_model, top_origin, top_focal, top_principal)
    for _ in range(depth):
        improved = False
        candidates = []
        for i in range(3):
            for frac in np.linspace(-1, 1, steps):
                cand = list(best_rvec)
                cand[i] = best_rvec[i] + frac * half_range
                candidates.append(cand)
        for cand in candidates:
            score = total_residual(cand, cameras_pixels, bottom_model, top_origin, top_focal, top_principal)
            if score < best_score:
                best_score = score
                best_rvec = cand
                improved = True
        half_range /= 3.0
        if not improved:
            continue
    return best_rvec, best_score


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--db', default=sweep_db.DEFAULT_DB_PATH)
    parser.add_argument('--sweep-id', default='latest')
    parser.add_argument('--pylon-id', default='A')
    parser.add_argument('--spacing-mm', type=float, default=geom.PYLON_CAMERA_SPACING_MM)
    args = parser.parse_args()

    conn = sweep_db.connect(args.db)
    sweep_id = int(args.sweep_id) if args.sweep_id != 'latest' else \
        conn.execute("SELECT MAX(sweep_id) FROM sweeps").fetchone()[0]

    placement = conn.execute(
        "SELECT capture_width, capture_height FROM pylon_placements WHERE sweep_id=? AND pylon_id=?",
        (sweep_id, args.pylon_id)).fetchone()
    width, height = placement

    rows = conn.execute('''
        SELECT b.pixel_x, b.pixel_y, t.pixel_x, t.pixel_y
        FROM raw_observations t
        JOIN raw_observations b ON t.led_position=b.led_position AND t.sweep_id=b.sweep_id AND t.pylon_id=b.pylon_id
        WHERE t.sweep_id=? AND t.pylon_id=? AND t.camera_position="top" AND b.camera_position="bottom"
          AND t.found=1 AND b.found=1
    ''', (sweep_id, args.pylon_id)).fetchall()
    conn.close()

    bottom_model, top_model = geom.make_pylon_cameras(width, height, spacing_mm=args.spacing_mm)
    top_origin = top_model.origin
    top_focal = top_model.focal_px
    top_principal = (top_model.principal_x, top_model.principal_y)

    baseline_score = total_residual([0, 0, 0], rows, bottom_model, top_origin, top_focal, top_principal)
    print(f"n={len(rows)} points, sweep {sweep_id} pylon {args.pylon_id}")
    print(f"baseline (no rotation correction): sum_sq_residual={baseline_score:.1f}  "
          f"rms_per_ray={np.sqrt(baseline_score/(2*len(rows))):.2f}mm")

    best_rvec, best_score = grid_search(
        rows, bottom_model, top_origin, top_focal, top_principal,
        center=[0.0, 0.0, 0.0], half_range=0.05, steps=7, depth=6)

    print(f"fitted rvec (radians): {best_rvec}")
    print(f"fitted rvec (degrees): {[np.degrees(v) for v in best_rvec]}")
    print(f"corrected: sum_sq_residual={best_score:.1f}  "
          f"rms_per_ray={np.sqrt(best_score/(2*len(rows))):.2f}mm")
    print(f"improvement: {100*(1-best_score/baseline_score):.1f}%")


if __name__ == "__main__":
    main()
