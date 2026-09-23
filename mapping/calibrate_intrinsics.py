"""
Fits one camera's real intrinsics (focal length, principal point,
distortion coefficients) from a set of ChArUco board captures - see
capture_calibration_images.py to collect those first.

This is what pylon_geometry.py's triangulation has been missing: it
currently assumes a focal length derived from the camera's spec'd FOV
and zero lens distortion. An earlier attempt to fit distortion
alongside noisy LED position data (not against an independently-known
target) was rightly rejected as overfitting-prone. This fits intrinsics
against a target whose true geometry is exactly known (the board's
real, measured square/marker size), which is the actual right way to
solve it - see the 2026-09-22/23 session notes on the tilt-fit residual
regression that motivated this.

Reprojection error (reported below) is a genuine accuracy metric here,
not just internal consistency like ray_residual_mm - it's checked
against the board's known true geometry. Sub-1px RMS is normal for a
good calibration set; several px suggests too few poses, poor pose
variety, or a soft/warped board.

Usage (venv active, run from mapping/):
    python3 calibrate_intrinsics.py --images-dir calib_images/cam0 --camera-id 0 \\
        --out camera_intrinsics_0.json
"""
import argparse
import glob
import json
import os

import cv2
import numpy as np

import calibration_board as calib


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--images-dir', required=True)
    parser.add_argument('--camera-id', type=int, required=True,
                         help="used only to find pose*_cam{id}.jpg files and to label the output")
    parser.add_argument('--out', default=None,
                         help="default: camera_intrinsics_{camera_id}.json in the current directory")
    args = parser.parse_args()

    board = calib.make_board()
    detector = calib.make_detector()

    image_paths = sorted(glob.glob(os.path.join(args.images_dir, f"*_cam{args.camera_id}.jpg")))
    if not image_paths:
        print(f"No images matching *_cam{args.camera_id}.jpg found in {args.images_dir}")
        return
    print(f"Found {len(image_paths)} candidate images for camera {args.camera_id}")

    all_obj_points = []
    all_img_points = []
    image_size = None
    used = 0
    for path in image_paths:
        img = cv2.imread(path)
        if img is None:
            print(f"  {os.path.basename(path)}: failed to read, skipping")
            continue
        if image_size is None:
            image_size = (img.shape[1], img.shape[0])
        gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
        charuco_corners, charuco_ids, _marker_corners, _marker_ids = detector.detectBoard(gray)
        if charuco_ids is None or len(charuco_ids) < calib.MIN_CORNERS_FOR_CAPTURE:
            n = 0 if charuco_ids is None else len(charuco_ids)
            print(f"  {os.path.basename(path)}: only {n} corners detected, skipping")
            continue
        obj_points, img_points = board.matchImagePoints(charuco_corners, charuco_ids)
        all_obj_points.append(obj_points)
        all_img_points.append(img_points)
        used += 1
        print(f"  {os.path.basename(path)}: {len(charuco_ids)} corners - used")

    if used < 8:
        print(f"\nOnly {used} usable images - too few for a reliable calibration (aim for 15-20+). "
              f"Capture more poses before fitting.")
        return

    print(f"\nCalibrating from {used} usable poses...")
    rms_error, camera_matrix, dist_coeffs, _rvecs, _tvecs = cv2.calibrateCamera(
        all_obj_points, all_img_points, image_size, None, None)

    fx, fy = camera_matrix[0, 0], camera_matrix[1, 1]
    cx, cy = camera_matrix[0, 2], camera_matrix[1, 2]
    print(f"RMS reprojection error: {rms_error:.3f}px "
          f"({'looks good' if rms_error < 1.0 else 'higher than ideal - consider more/better poses'})")
    print(f"fx={fx:.2f} fy={fy:.2f} cx={cx:.2f} cy={cy:.2f}")
    print(f"distortion coeffs (k1,k2,p1,p2,k3): {dist_coeffs.flatten()}")

    out_path = args.out or f"camera_intrinsics_{args.camera_id}.json"
    with open(out_path, 'w') as f:
        json.dump({
            "camera_id": args.camera_id,
            "image_width": image_size[0],
            "image_height": image_size[1],
            "camera_matrix": camera_matrix.tolist(),
            "dist_coeffs": dist_coeffs.flatten().tolist(),
            "rms_reprojection_error_px": rms_error,
            "num_poses_used": used,
        }, f, indent=2)
    print(f"\nSaved to {out_path}")


if __name__ == "__main__":
    main()
