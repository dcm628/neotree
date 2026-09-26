"""
Fits each camera's real intrinsics (focal length, principal point, lens
distortion) from a capture session (capture_calibration_images.py), against
the board's exactly-known geometry.

This is what pylon_geometry.py's triangulation has been missing: it assumed
a focal length from the camera's spec'd FOV and no distortion. An earlier
attempt to fit distortion against noisy LED data was rightly rejected as
overfitting; fitting against a known target is the right way.

Per camera: uses the poses that were new for it (not repeats), fits, drops
views whose own reprojection error is far above the rest (a blurred or
misdetected frame) and fits again. Reprojection error is a real accuracy
measure here - it's against the board's true geometry. Well under 1px RMS
is normal for a good set.

Writes calibration/intrinsics_<serial>.json per camera, recording the
resolution and focus the images were taken at (the fit is only valid for
those).

Usage (venv active, from mapping/):
    python3 calibrate_intrinsics.py --session calib_images/session1
    python3 calibrate_intrinsics.py --session calib_images/session1 --serial 846EA2EF
"""
import argparse
import json
import os

import cv2
import numpy as np

import calibration_board as calib

OUTLIER_FACTOR = 3.0   # a view this many times the median view error is dropped


def load_manifest(session):
    with open(os.path.join(session, "manifest.json")) as f:
        return json.load(f)


def views_for(session, manifest, serial, detector, board, new_only=True):
    """(object points, image points, name) for each usable pose of one camera."""
    views = []
    for p in manifest["poses"]:
        if serial not in (p["new"] if new_only else p["saved"]):
            continue
        name = f"pose{p['pose']:03d}_{serial}.jpg"
        img = cv2.imread(os.path.join(session, name))
        if img is None:
            continue
        corners, ids, _mc, _mi = detector.detectBoard(cv2.cvtColor(img, cv2.COLOR_BGR2GRAY))
        if ids is None or len(ids) < calib.MIN_CORNERS_FOR_CAPTURE:
            continue
        obj, pts = board.matchImagePoints(corners, ids)
        views.append((obj, pts, name))
    return views


def fit(views, size):
    obj = [v[0] for v in views]
    pts = [v[1] for v in views]
    rms, K, dist, _r, _t, _std_in, _std_ex, per_view = cv2.calibrateCameraExtended(obj, pts, size, None, None)
    return rms, K, dist, per_view.flatten()


def calibrate_camera(session, manifest, cam, out_dir):
    board = calib.make_board()
    detector = calib.make_detector()
    size = tuple(manifest["resolution"])
    serial = cam["serial"]
    views = views_for(session, manifest, serial, detector, board)
    print(f"\ncamera {cam['id']} {cam['model']} ({serial}): {len(views)} usable views")
    if len(views) < 10:
        print("  too few views for a reliable fit (aim for 20-30) - skipped")
        return None

    rms, K, dist, per_view = fit(views, size)
    median = float(np.median(per_view))
    keep = [v for v, e in zip(views, per_view) if e <= OUTLIER_FACTOR * max(median, 0.1)]
    dropped = [(v[2], e) for v, e in zip(views, per_view) if e > OUTLIER_FACTOR * max(median, 0.1)]
    if dropped:
        print(f"  dropped {len(dropped)} outlier view(s): " + ", ".join(f"{n} ({e:.2f}px)" for n, e in dropped))
        rms, K, dist, per_view = fit(keep, size)

    fx, fy, cx, cy = K[0, 0], K[1, 1], K[0, 2], K[1, 2]
    hfov = np.degrees(2 * np.arctan(size[0] / (2 * fx)))
    vfov = np.degrees(2 * np.arctan(size[1] / (2 * fy)))
    dfov = np.degrees(2 * np.arctan(np.hypot(*size) / (2 * (fx + fy) / 2)))
    verdict = "good" if rms < 0.5 else "fair" if rms < 1.0 else "poor - more/better poses needed"
    print(f"  RMS reprojection error {rms:.3f}px ({verdict}), worst view {per_view.max():.2f}px")
    print(f"  fx={fx:.1f} fy={fy:.1f} cx={cx:.1f} cy={cy:.1f} (image centre {size[0] / 2:.0f},{size[1] / 2:.0f})")
    print(f"  field of view {hfov:.1f} x {vfov:.1f} deg, diagonal {dfov:.1f} (the model assumed 78)")
    print(f"  distortion k1,k2,p1,p2,k3 = {np.round(dist.flatten(), 4).tolist()}")

    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, f"intrinsics_{serial}.json")
    with open(out, "w") as f:
        json.dump({
            "serial": serial, "model": cam["model"],
            "image_width": size[0], "image_height": size[1], "focus": manifest["focus"],
            "camera_matrix": K.tolist(), "dist_coeffs": dist.flatten().tolist(),
            "rms_reprojection_error_px": rms, "num_views": len(keep),
            "diagonal_fov_deg": dfov, "session": os.path.basename(os.path.normpath(session)),
        }, f, indent=2)
    print(f"  saved {out}")
    return rms


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--session', required=True, help="a capture_calibration_images.py output directory")
    parser.add_argument('--serial', default=None, help="just this camera (default: every camera in the session)")
    parser.add_argument('--out-dir', default="calibration")
    args = parser.parse_args()

    manifest = load_manifest(args.session)
    for cam in manifest["cameras"]:
        if args.serial is None or cam["serial"] == args.serial:
            calibrate_camera(args.session, manifest, cam, args.out_dir)


if __name__ == "__main__":
    main()
