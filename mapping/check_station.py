"""
Checks a pylon's stereo calibration at a station (docs/CALIBRATION.md,
"Phased mapping"): moving a whole pylon keeps its stereo geometry, but a
bumped camera doesn't. A short board capture at the station is compared
with the stored calibration:

  - the board's pose from the bottom camera, carried to the top camera
    through the stored stereo geometry, should land where the top camera
    actually sees the board's corners (reprojection, in pixels);
  - the board's corners triangulated through the stored geometry should
    be the board's true size (in millimetres).

Capture first (8 or so poses both cameras of the pylon see):
    python3 capture_calibration_images.py --out-dir calib_images/check_S3 \\
        --cameras 0 2 --pylon A:0:2 --focus 60 --setup phase1 --target 8 --quick
then:
    python3 check_station.py --session calib_images/check_S3
"""
import argparse
import itertools
import json
import os

import cv2
import numpy as np

import calibration_board as calib
import pylon_geometry as geom

OK_PX = 1.0       # median reprojection through the stored stereo
OK_MM = 0.5       # median board-size error


def check_pylon(session, manifest, pylon, calib_dir):
    focus, setup = manifest["focus"], manifest.get("setup")
    top, bottom = pylon["top"], pylon["bottom"]
    ib, it = geom.load_intrinsics(bottom, calib_dir, focus), geom.load_intrinsics(top, calib_dir, focus)
    stereo = geom.find_stereo(bottom, top, calib_dir, setup)
    if ib is None or it is None or stereo is None:
        print(f"pylon {pylon['name']}: no calibration for these cameras at focus {focus}, setup {setup}")
        return None
    Kb, Db = np.array(ib["camera_matrix"]), np.array(ib["dist_coeffs"])
    Kt, Dt = np.array(it["camera_matrix"]), np.array(it["dist_coeffs"])
    R, T = np.array(stereo["rotation_matrix"]), np.array(stereo["translation_mm"]) / 1000.0
    board, detector = calib.make_board(), calib.make_detector()
    corners3d = board.getChessboardCorners()

    def detect(name):
        img = cv2.imread(os.path.join(session, name))
        if img is None:
            return {}
        c, ids, _a, _b = detector.detectBoard(cv2.cvtColor(img, cv2.COLOR_BGR2GRAY))
        return {} if ids is None else {int(i): p.reshape(2) for i, p in zip(ids.flatten(), c)}

    px, mm = [], []
    for p in manifest["poses"]:
        if bottom not in p["saved"] or top not in p["saved"]:
            continue
        db = detect(f"pose{p['pose']:03d}_{bottom}.jpg")
        dt = detect(f"pose{p['pose']:03d}_{top}.jpg")
        common = sorted(set(db) & set(dt))
        if len(common) < 15:
            continue
        obj = np.array([corners3d[i] for i in common], dtype=np.float64)
        ok, rvec, tvec = cv2.solvePnP(obj, np.array([db[i] for i in common]), Kb, Db)
        if not ok:
            continue
        # The board in the top camera, through the stored stereo.
        Rb = cv2.Rodrigues(rvec)[0]
        Rt, tt = R @ Rb, R @ tvec.ravel() + T
        proj = cv2.projectPoints(obj, cv2.Rodrigues(Rt)[0], tt, Kt, Dt)[0].reshape(-1, 2)
        px.append(float(np.sqrt(np.mean(np.sum((proj - np.array([dt[i] for i in common])) ** 2, axis=1)))))
        # The board's size, triangulated.
        nb = cv2.undistortPoints(np.array([db[i] for i in common]).reshape(-1, 1, 2), Kb, Db).reshape(-1, 2)
        nt = cv2.undistortPoints(np.array([dt[i] for i in common]).reshape(-1, 1, 2), Kt, Dt).reshape(-1, 2)
        Xh = cv2.triangulatePoints(np.hstack([np.eye(3), np.zeros((3, 1))]), np.hstack([R, T.reshape(3, 1)]),
                                   nb.T, nt.T)
        X = (Xh[:3] / Xh[3]).T * 1000.0
        errs = [np.linalg.norm(X[a] - X[b]) - np.linalg.norm(corners3d[common[a]] - corners3d[common[b]]) * 1000
                for a, b in itertools.combinations(range(len(common)), 2)
                if np.linalg.norm(corners3d[common[a]] - corners3d[common[b]]) >= 0.1]
        mm.append(float(np.median(np.abs(errs))))

    if len(px) < 3:
        print(f"pylon {pylon['name']}: only {len(px)} poses both cameras saw - capture a few more")
        return None
    med_px, med_mm = float(np.median(px)), float(np.median(mm))
    ok = med_px < OK_PX and med_mm < OK_MM
    print(f"pylon {pylon['name']} ({len(px)} poses): through the stored stereo the top camera is off by "
          f"{med_px:.2f} px (calibrated: {stereo['rms_reprojection_error_px']:.2f}); board size off by {med_mm:.2f} mm")
    print("  OK - the stereo calibration still holds" if ok else
          "  CHANGED - a camera has moved: capture and fit this pylon's stereo again for this setup")
    return ok


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--session', required=True)
    parser.add_argument('--calib-dir', default=geom.DEFAULT_CALIBRATION_DIR)
    args = parser.parse_args()
    with open(os.path.join(args.session, "manifest.json")) as f:
        manifest = json.load(f)
    for pylon in manifest["pylons"]:
        check_pylon(args.session, manifest, pylon, args.calib_dir)


if __name__ == "__main__":
    main()
