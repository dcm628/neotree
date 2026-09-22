"""
Fits a small rotation correction for the top camera plus a shared radial
lens-distortion coefficient (relative to the parallel-cameras, no-
distortion assumption in pylon_geometry.py) by minimizing total ray
residual across a sweep's real LED point correspondences - self-
calibration using the LEDs themselves as calibration targets, since we
don't have a checkerboard calibration set up.

Rotation alone (fit previously) cannot correct for a systematic bias that
affects both cameras' rays the same correlated way (e.g. lens distortion)
- that still intersects with low residual, just at the wrong point, so it
survives a residual-based outlier filter undetected. A weak-to-moderate
correlation (r=0.365) between ray residual and distance from image center
was observed on sweep 2, consistent with uncorrected distortion - this
fits a simple one-parameter radial model (k1, applied to both cameras
identically since they're the same lens) jointly with the rotation to see
how much it helps beyond the rotation alone.

Coarse-to-fine grid search over a 4-parameter vector [rx, ry, rz, k1]
(Rodrigues rotation + shared distortion) - no scipy available on the Pi,
and 4 parameters over a small smooth objective doesn't need gradients.

Usage (venv active):
    python3 fit_camera_tilt.py --sweep-id 2 --pylon-id A
"""
import argparse

import cv2
import numpy as np

import pylon_geometry as geom
import sweep_db


def distorted_ray_components(u, v, focal, principal, k1):
    """Normalized (right, up) for pixel (u, v), with a simple radial
    undistortion correction applied (see pylon_geometry note on why this
    is only an approximate inverse, fine for a small k1)."""
    right = (v - principal[1]) / focal
    up = (u - principal[0]) / focal
    r2 = right * right + up * up
    scale = 1.0 - k1 * r2
    return right * scale, up * scale


def total_residual(params, cameras_pixels, bottom_model, top_origin, focal, bottom_principal, top_principal):
    rx, ry, rz, k1 = params
    R, _ = cv2.Rodrigues(np.array([rx, ry, rz], dtype=float))
    total = 0.0
    for bu, bv, tu, tv in cameras_pixels:
        br, bup = distorted_ray_components(bu, bv, focal, bottom_principal, k1)
        d_bottom = np.array([br, 1.0, bup]); d_bottom /= np.linalg.norm(d_bottom)

        tr, tup = distorted_ray_components(tu, tv, focal, top_principal, k1)
        d_top_ideal = np.array([tr, 1.0, tup]); d_top_ideal /= np.linalg.norm(d_top_ideal)
        d_top = R @ d_top_ideal

        origins = [bottom_model.origin, top_origin]
        directions = [d_bottom, d_top]
        A = np.zeros((3, 3)); b = np.zeros(3)
        for o, d in zip(origins, directions):
            M = np.eye(3) - np.outer(d, d)
            A += M; b += M @ o
        pt, *_ = np.linalg.lstsq(A, b, rcond=None)
        for o, d in zip(origins, directions):
            v_ = pt - o
            perp = v_ - np.dot(v_, d) * d
            total += np.dot(perp, perp)
    return total


def grid_search(cameras_pixels, bottom_model, top_origin, focal, bottom_principal, top_principal,
                 center, half_ranges, steps, depth):
    n = len(center)
    best = list(center)
    best_score = total_residual(best, cameras_pixels, bottom_model, top_origin, focal, bottom_principal, top_principal)
    ranges = list(half_ranges)
    for _ in range(depth):
        improved = False
        for i in range(n):
            for frac in np.linspace(-1, 1, steps):
                cand = list(best)
                cand[i] = best[i] + frac * ranges[i]
                score = total_residual(cand, cameras_pixels, bottom_model, top_origin, focal, bottom_principal, top_principal)
                if score < best_score:
                    best_score = score
                    best = cand
                    improved = True
        ranges = [r / 3.0 for r in ranges]
        if not improved:
            continue
    return best, best_score


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--db', default=sweep_db.DEFAULT_DB_PATH)
    parser.add_argument('--sweep-id', default='latest')
    parser.add_argument('--pylon-id', default='A')
    parser.add_argument('--spacing-mm', type=float, default=geom.PYLON_CAMERA_SPACING_MM)
    parser.add_argument('--fit-distortion', action='store_true',
                         help="also try jointly fitting a shared radial distortion coefficient "
                              "(experimental - a prior attempt overfit a small dataset and was "
                              "rejected after cross-checking against a known real measurement; "
                              "not part of the standard per-sweep fit, print-only, never saved)")
    parser.add_argument('--no-save', action='store_true',
                         help="print the fit without writing it to pylon_tilt_fits")
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

    bottom_model, top_model = geom.make_pylon_cameras(width, height, spacing_mm=args.spacing_mm,
                                                        top_rotation_rvec=None)
    top_origin = top_model.origin
    focal = top_model.focal_px
    bottom_principal = (bottom_model.principal_x, bottom_model.principal_y)
    top_principal = (top_model.principal_x, top_model.principal_y)

    baseline_score = total_residual([0, 0, 0, 0.0], rows, bottom_model, top_origin, focal, bottom_principal, top_principal)
    n = len(rows)
    print(f"n={n} points, sweep {sweep_id} pylon {args.pylon_id}")
    print(f"baseline (no correction): sum_sq={baseline_score:.1f}  rms_per_ray={np.sqrt(baseline_score/(2*n)):.2f}mm")

    # Standard per-sweep, per-pylon correction: fit the top camera's
    # rotation relative to the bottom camera fresh every time, since the
    # rig is deliberately hand-aimed (not kept parallel) each sweep - see
    # module docstring / project discussion.
    rot_only, rot_score = grid_search(
        rows, bottom_model, top_origin, focal, bottom_principal, top_principal,
        center=[0.0, 0.0, 0.0, 0.0], half_ranges=[0.05, 0.05, 0.05, 0.0], steps=7, depth=6)
    rms = float(np.sqrt(rot_score / (2 * n)))
    print(f"fitted rotation: rvec_deg={[round(np.degrees(v),3) for v in rot_only[:3]]}  "
          f"rms_per_ray={rms:.2f}mm  (improvement over baseline: {100*(1-rot_score/baseline_score):.1f}%)")

    if not args.no_save:
        sweep_db.record_pylon_tilt_fit(conn, sweep_id, args.pylon_id, rot_only[:3], n, rms)
        print(f"Saved to pylon_tilt_fits (sweep {sweep_id}, pylon {args.pylon_id})")
    conn.close()

    if args.fit_distortion:
        joint, joint_score = grid_search(
            rows, bottom_model, top_origin, focal, bottom_principal, top_principal,
            center=[rot_only[0], rot_only[1], rot_only[2], 0.0],
            half_ranges=[0.02, 0.02, 0.02, 0.5], steps=9, depth=6)
        print(f"[experimental, not saved] rotation+distortion: "
              f"rvec_deg={[round(np.degrees(v),3) for v in joint[:3]]}  k1={joint[3]:.4f}  "
              f"rms_per_ray={np.sqrt(joint_score/(2*n)):.2f}mm  "
              f"(improvement over rotation-only: {100*(1-joint_score/rot_score):.1f}%)")


if __name__ == "__main__":
    main()
