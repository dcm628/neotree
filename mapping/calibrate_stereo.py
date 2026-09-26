"""
Fits each pylon's true stereo geometry - where its top camera is and which
way it points, relative to the bottom camera - from the poses of a capture
session (capture_calibration_images.py) that both of its cameras saw, using
each camera's intrinsics (calibrate_intrinsics.py, run first), held fixed.

Replaces the assumed geometry (cameras parallel, 592mm apart, plus a small
rotation fitted per sweep from noisy LED data) with a measurement. It holds
only while neither camera of the pylon is re-aimed or moved on its rail:
if one is, capture and fit again. (Moving the whole pylon is fine - that's
what cross-sweep alignment handles.)

Then an independent accuracy check, in millimetres: the board's corners are
triangulated from the two cameras and the distances between them compared
with the board's true geometry, pose by pose - the same triangulation the
LEDs get, against a known answer.

Convention: a point X in the bottom camera's frame (OpenCV: x right, y
down, z forward in its raw image) is R @ X + T in the top camera's frame.

Usage (venv active, from mapping/):
    python3 calibrate_stereo.py --session calib_images/session1            # every pylon
    python3 calibrate_stereo.py --session calib_images/session1 --pylon A
"""
import argparse
import itertools
import json
import os

import cv2
import numpy as np

import calibration_board as calib


def load_intrinsics(calib_dir, serial, resolution):
    path = os.path.join(calib_dir, f"intrinsics_{serial}.json")
    with open(path) as f:
        d = json.load(f)
    if [d["image_width"], d["image_height"]] != list(resolution):
        raise SystemExit(f"{path} is for {d['image_width']}x{d['image_height']}, the session is {resolution}")
    return np.array(d["camera_matrix"]), np.array(d["dist_coeffs"])


def detections(session, name, detector):
    img = cv2.imread(os.path.join(session, name))
    if img is None:
        return {}
    corners, ids, _mc, _mi = detector.detectBoard(cv2.cvtColor(img, cv2.COLOR_BGR2GRAY))
    if ids is None:
        return {}
    return {int(i): c.reshape(2) for i, c in zip(ids.flatten(), corners)}


def triangulate(K1, D1, K2, D2, R, T, pts1, pts2):
    """3D points (mm, bottom camera frame) from matching pixels in both cameras."""
    n1 = cv2.undistortPoints(pts1.reshape(-1, 1, 2), K1, D1).reshape(-1, 2)
    n2 = cv2.undistortPoints(pts2.reshape(-1, 1, 2), K2, D2).reshape(-1, 2)
    P1 = np.hstack([np.eye(3), np.zeros((3, 1))])
    P2 = np.hstack([R, T.reshape(3, 1)])
    X = cv2.triangulatePoints(P1, P2, n1.T, n2.T)
    return (X[:3] / X[3]).T * 1000.0


def calibrate_pylon(session, manifest, pylon, calib_dir, min_common):
    resolution = manifest["resolution"]
    top, bottom = pylon["top"], pylon["bottom"]
    K1, D1 = load_intrinsics(calib_dir, bottom, resolution)
    K2, D2 = load_intrinsics(calib_dir, top, resolution)
    board = calib.make_board()
    detector = calib.make_detector()
    board_3d = board.getChessboardCorners() * 1000.0   # mm, by corner id

    obj, img1, img2, used = [], [], [], []
    for p in manifest["poses"]:
        if bottom not in p["saved"] or top not in p["saved"]:
            continue
        d1 = detections(session, f"pose{p['pose']:03d}_{bottom}.jpg", detector)
        d2 = detections(session, f"pose{p['pose']:03d}_{top}.jpg", detector)
        common = sorted(set(d1) & set(d2))
        if len(common) < min_common:
            continue
        obj.append(np.array([board_3d[i] / 1000.0 for i in common], dtype=np.float32))
        img1.append(np.array([d1[i] for i in common], dtype=np.float32))
        img2.append(np.array([d2[i] for i in common], dtype=np.float32))
        used.append((p["pose"], common))

    print(f"\npylon {pylon['name']} (top {top}, bottom {bottom}): {len(used)} poses seen by both cameras")
    if len(used) < 8:
        print("  too few for a reliable fit (aim for 15+ poses both cameras see) - skipped")
        return None

    rms, _K1, _D1, _K2, _D2, R, T, _E, _F = cv2.stereoCalibrate(
        obj, img1, img2, K1, D1, K2, D2, tuple(resolution), flags=cv2.CALIB_FIX_INTRINSIC)
    top_center = (-R.T @ T).flatten() * 1000.0
    angle = np.degrees(np.linalg.norm(cv2.Rodrigues(R)[0]))
    print(f"  RMS reprojection error {rms:.3f}px")
    print(f"  top camera centre, in the bottom camera's frame: {np.round(top_center, 1).tolist()} mm "
          f"(baseline {np.linalg.norm(top_center):.1f} mm; the model assumed 592)")
    print(f"  top camera turned {angle:.2f} deg relative to the bottom one")

    # The accuracy check: distances between triangulated corners vs the board's.
    print("  accuracy check - board corners triangulated, distances vs the real board:")
    all_err = []
    rows = []
    for (pose, common), p1, p2 in zip(used, img1, img2):
        X = triangulate(K1, D1, K2, D2, R, T.flatten(), p1, p2)
        errs = []
        for a, b in itertools.combinations(range(len(common)), 2):
            true = np.linalg.norm(board_3d[common[a]] - board_3d[common[b]])
            if true >= 100.0:   # pairs at least 100mm apart: scale errors show
                errs.append(np.linalg.norm(X[a] - X[b]) - true)
        errs = np.array(errs)
        depth = float(np.mean(X[:, 2]))
        rows.append((depth, float(np.sqrt(np.mean(errs ** 2))), float(np.mean(errs))))
        all_err.extend(errs.tolist())
    rows.sort()
    for depth, rmse, bias in rows:
        print(f"    pose at {depth / 1000:.2f} m: RMS {rmse:.2f} mm, mean {bias:+.2f} mm")
    all_err = np.array(all_err)
    print(f"  overall: RMS {np.sqrt(np.mean(all_err ** 2)):.2f} mm over {len(all_err)} corner pairs "
          f"(100-{np.linalg.norm(board_3d.max(0) - board_3d.min(0)):.0f} mm apart)")

    out = os.path.join(calib_dir, f"stereo_{pylon['name']}.json")
    with open(out, "w") as f:
        json.dump({
            "pylon": pylon["name"], "top_serial": top, "bottom_serial": bottom,
            "image_width": resolution[0], "image_height": resolution[1],
            "rotation_matrix": R.tolist(), "translation_mm": (T.flatten() * 1000.0).tolist(),
            "top_center_in_bottom_frame_mm": top_center.tolist(),
            "baseline_mm": float(np.linalg.norm(top_center)),
            "rms_reprojection_error_px": rms, "num_poses": len(used),
            "check_rms_mm": float(np.sqrt(np.mean(all_err ** 2))),
            "session": os.path.basename(os.path.normpath(session)),
        }, f, indent=2)
    print(f"  saved {out}")
    return rms


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--session', required=True)
    parser.add_argument('--pylon', default=None, help="just this pylon (default: every pylon in the session)")
    parser.add_argument('--calib-dir', default="calibration")
    parser.add_argument('--min-common-corners', type=int, default=15)
    args = parser.parse_args()

    with open(os.path.join(args.session, "manifest.json")) as f:
        manifest = json.load(f)
    for pylon in manifest["pylons"]:
        if args.pylon is None or pylon["name"] == args.pylon:
            calibrate_pylon(args.session, manifest, pylon, args.calib_dir, args.min_common_corners)


if __name__ == "__main__":
    main()
