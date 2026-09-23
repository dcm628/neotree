"""
Fits the TRUE relative pose (rotation + translation) between a pylon's
top and bottom cameras from simultaneous ChArUco captures of both - see
capture_calibration_images.py's --camera-id/--camera-id-2 stereo mode
to collect those first, and calibrate_intrinsics.py to fit each
camera's own intrinsics first (this reuses them, fixed, rather than
re-fitting them jointly with the stereo geometry).

Replaces the current ad hoc "assume spacing_mm vertical offset, fit a
small rotation per sweep from noisy LED data" approach
(pylon_geometry.py / fit_camera_tilt.py) with a real measurement of the
rig's actual geometry - which shouldn't change between pylon
repositions (only the rig's placement relative to the tree does, which
per-sweep tilt-fit and cross-sweep Kabsch alignment already handle
separately).

Usage (venv active, run from mapping/):
    python3 calibrate_stereo.py --images-dir calib_images/pylonA_stereo \\
        --camera-id 0 --camera-id-2 2 \\
        --intrinsics-1 camera_intrinsics_0.json --intrinsics-2 camera_intrinsics_2.json \\
        --out stereo_extrinsics_pylonA.json
"""
import argparse
import glob
import json
import os

import cv2
import numpy as np

import calibration_board as calib


def load_intrinsics(path):
    with open(path) as f:
        data = json.load(f)
    K = np.array(data["camera_matrix"], dtype=np.float64)
    dist = np.array(data["dist_coeffs"], dtype=np.float64)
    return K, dist, (data["image_width"], data["image_height"])


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--images-dir', required=True)
    parser.add_argument('--camera-id', type=int, required=True, help="first (e.g. top) camera id")
    parser.add_argument('--camera-id-2', type=int, required=True, help="second (e.g. bottom) camera id")
    parser.add_argument('--intrinsics-1', required=True)
    parser.add_argument('--intrinsics-2', required=True)
    parser.add_argument('--min-common-corners', type=int, default=15,
                         help="a pose only counts if both cameras share at least this many "
                              "detected corner IDs - fewer than this under-constrains that pose's "
                              "contribution to the stereo geometry")
    parser.add_argument('--out', default=None)
    args = parser.parse_args()

    K1, D1, size1 = load_intrinsics(args.intrinsics_1)
    K2, D2, size2 = load_intrinsics(args.intrinsics_2)
    if size1 != size2:
        print(f"Warning: camera resolutions differ ({size1} vs {size2}) - stereoCalibrate assumes "
              f"a shared image size; using {size1}")

    board = calib.make_board()
    detector = calib.make_detector()
    board_corners_3d = board.getChessboardCorners()  # Nx3, indexed by charuco corner id

    pose_ids = sorted({
        os.path.basename(p).split('_')[0][len('pose'):]
        for p in glob.glob(os.path.join(args.images_dir, f"pose*_cam{args.camera_id}.jpg"))
    })
    print(f"Found {len(pose_ids)} candidate pose(s)")

    obj_points_list = []
    img_points_1_list = []
    img_points_2_list = []
    used = 0
    for pose_id in pose_ids:
        path1 = os.path.join(args.images_dir, f"pose{pose_id}_cam{args.camera_id}.jpg")
        path2 = os.path.join(args.images_dir, f"pose{pose_id}_cam{args.camera_id_2}.jpg")
        if not (os.path.exists(path1) and os.path.exists(path2)):
            continue
        img1 = cv2.imread(path1)
        img2 = cv2.imread(path2)
        gray1 = cv2.cvtColor(img1, cv2.COLOR_BGR2GRAY)
        gray2 = cv2.cvtColor(img2, cv2.COLOR_BGR2GRAY)

        corners1, ids1, _mc1, _mi1 = detector.detectBoard(gray1)
        corners2, ids2, _mc2, _mi2 = detector.detectBoard(gray2)
        if ids1 is None or ids2 is None:
            print(f"  pose{pose_id}: board not detected in one or both cameras, skipping")
            continue

        map1 = {int(i): c for i, c in zip(ids1.flatten(), corners1)}
        map2 = {int(i): c for i, c in zip(ids2.flatten(), corners2)}
        common_ids = sorted(set(map1) & set(map2))
        if len(common_ids) < args.min_common_corners:
            print(f"  pose{pose_id}: only {len(common_ids)} common corners, skipping")
            continue

        obj_points_list.append(np.array([board_corners_3d[i] for i in common_ids], dtype=np.float32))
        img_points_1_list.append(np.array([map1[i] for i in common_ids], dtype=np.float32))
        img_points_2_list.append(np.array([map2[i] for i in common_ids], dtype=np.float32))
        used += 1
        print(f"  pose{pose_id}: {len(common_ids)} common corners - used")

    if used < 8:
        print(f"\nOnly {used} usable pose pairs - too few for a reliable stereo fit (aim for 15-20+).")
        return

    print(f"\nStereo-calibrating from {used} usable pose pairs...")
    rms_error, K1_out, D1_out, K2_out, D2_out, R, T, _E, _F = cv2.stereoCalibrate(
        obj_points_list, img_points_1_list, img_points_2_list,
        K1, D1, K2, D2, size1, flags=cv2.CALIB_FIX_INTRINSIC)

    baseline_mm = float(np.linalg.norm(T) * 1000.0)
    print(f"RMS reprojection error: {rms_error:.3f}px")
    print(f"Fitted baseline (camera separation): {baseline_mm:.1f}mm")
    print(f"Rotation matrix (camera 2 relative to camera 1):\n{R}")
    print(f"Translation (camera 2 relative to camera 1, mm): {(T.flatten() * 1000.0)}")

    out_path = args.out or f"stereo_extrinsics_{args.camera_id}_{args.camera_id_2}.json"
    with open(out_path, 'w') as f:
        json.dump({
            "camera_id_1": args.camera_id,
            "camera_id_2": args.camera_id_2,
            "rotation_matrix": R.tolist(),
            "translation_mm": (T.flatten() * 1000.0).tolist(),
            "baseline_mm": baseline_mm,
            "rms_reprojection_error_px": rms_error,
            "num_poses_used": used,
        }, f, indent=2)
    print(f"\nSaved to {out_path}")


if __name__ == "__main__":
    main()
