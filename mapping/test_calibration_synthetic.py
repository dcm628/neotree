"""End-to-end check (run on the Pi, venv active, from mapping/: python3
test_calibration_synthetic.py [out-dir] [farthest pose mm]; ~2 minutes).

End-to-end check of the calibration scripts on synthetic images with known
truth: renders the real ChArUco board as seen by two pylons of distorting,
portrait-mounted cameras, writes a capture session like
capture_calibration_images.py does, then runs calibrate_intrinsics.py and
calibrate_stereo.py and compares what they fit with the truth, and
pylon_geometry's calibrated cameras with the true pylon geometry.

Found with it (2026-09-25): the scripts recover the truth (focal lengths
within ~0.2%, the top camera within ~1-2 mm), but the board is too small
to detect past ~1.2 m, and at the tree's 2-3 m even that small error
becomes ~7-15 mm of depth - so the board fit needs refining against the
LEDs themselves (both pylons jointly) for full accuracy."""
import json
import os
import subprocess
import sys

import cv2
import numpy as np

import calibration_board as calib
import pylon_geometry as geom

W, H = 1280, 720
rng = np.random.default_rng(7)
OUT = sys.argv[1] if len(sys.argv) > 1 else "synth_session"
CAL = OUT + "_cal"
FAR = float(sys.argv[2]) if len(sys.argv) > 2 else 3000

board = calib.make_board()
PX_PER_MM = 8
board_img = board.generateImage((int(calib.SQUARES_X * calib.CHECKER_MM * PX_PER_MM),
                                 int(calib.SQUARES_Y * calib.CHECKER_MM * PX_PER_MM)), marginSize=0)
board_w_mm = calib.SQUARES_X * calib.CHECKER_MM
board_h_mm = calib.SQUARES_Y * calib.CHECKER_MM

cams = {
    "SYN_A_TOP": (np.array([[935.0, 0, 648], [0, 931, 364], [0, 0, 1]]), np.array([0.09, -0.21, 0.001, -0.0015, 0.12])),
    "SYN_A_BOT": (np.array([[948.0, 0, 632], [0, 944, 352], [0, 0, 1]]), np.array([0.11, -0.25, -0.0008, 0.001, 0.15])),
}
# Top camera: 592 mm up (OpenCV x of the bottom camera = world up), turned a little.
R_true, _ = cv2.Rodrigues(np.array([0.004, -0.02, 0.035]))
C_top = np.array([592.0, 3.0, -6.0])          # centre, bottom camera frame, mm
T_true = -R_true @ C_top


def render(K, dist, R, t):
    """The board (plane z=0 in board mm coords, pose X_cam = R X + t) seen by camera K, dist."""
    u, v = np.meshgrid(np.arange(W, dtype=np.float32), np.arange(H, dtype=np.float32))
    pts = np.stack([u.ravel(), v.ravel()], 1).reshape(-1, 1, 2)
    n = cv2.undistortPoints(pts, K, dist).reshape(-1, 2)
    rays = np.hstack([n, np.ones((len(n), 1))])            # camera frame
    Rt = R.T
    o = -Rt @ t                                             # camera centre in board frame
    d = rays @ Rt.T                                         # ray directions in board frame
    s = -o[2] / d[:, 2]
    p = o + d * s[:, None]
    mx = (p[:, 0] * PX_PER_MM).reshape(H, W).astype(np.float32)
    my = (p[:, 1] * PX_PER_MM).reshape(H, W).astype(np.float32)
    bad = (s <= 0).reshape(H, W)
    mx[bad] = -1
    img = cv2.remap(board_img, mx, my, cv2.INTER_LINEAR, borderValue=90)
    img = cv2.GaussianBlur(img, (3, 3), 0.6)
    img = np.clip(img.astype(np.float32) + rng.normal(0, 2.0, img.shape), 0, 255).astype(np.uint8)
    return cv2.cvtColor(img, cv2.COLOR_GRAY2BGR)


def random_pose():
    """A board pose in the bottom camera's frame: somewhere in view, 0.8-3 m away, tilted."""
    dist_mm = rng.uniform(800, FAR)
    # Aim between the two cameras' views at far range, anywhere in one camera's view close up.
    x = rng.uniform(-0.45, 0.45) * dist_mm + rng.uniform(0, 600) if dist_mm < 1300 else rng.uniform(0, 600) + rng.uniform(-0.15, 0.15) * dist_mm
    y = rng.uniform(-0.3, 0.3) * dist_mm
    rvec = np.array([rng.uniform(-0.5, 0.5), rng.uniform(-0.5, 0.5), 0.0]) + np.array([0, 0, rng.uniform(-np.pi, np.pi)]) * (dist_mm < 1300)
    R, _ = cv2.Rodrigues(rvec)
    centre = np.array([x, y, dist_mm])
    t = centre - R @ np.array([board_w_mm / 2, board_h_mm / 2, 0])
    return R, t


os.makedirs(OUT, exist_ok=True)
detector = calib.make_detector()
manifest = {"resolution": [W, H], "focus": 30,
            "cameras": [{"id": i, "serial": s, "model": "synthetic", "usb_path": "-"} for i, s in enumerate(cams)],
            "pylons": [{"name": "A", "top": "SYN_A_TOP", "bottom": "SYN_A_BOT"}], "poses": []}
pose = 0
tries = 0
while not os.path.exists(os.path.join(OUT, "manifest.json")) and pose < 40 and tries < 400:
    tries += 1
    Rb, tb = random_pose()
    views = {"SYN_A_BOT": (Rb, tb), "SYN_A_TOP": (R_true @ Rb, R_true @ tb + T_true)}
    saved = []
    for serial, (R, t) in views.items():
        K, dist = cams[serial]
        img = render(K, dist, R, t)
        corners, ids, _a, _b = detector.detectBoard(cv2.cvtColor(img, cv2.COLOR_BGR2GRAY))
        if ids is not None and len(ids) >= calib.MIN_CORNERS_FOR_CAPTURE:
            cv2.imwrite(os.path.join(OUT, f"pose{pose:03d}_{serial}.jpg"), img)
            saved.append(serial)
    if saved:
        manifest["poses"].append({"pose": pose, "saved": saved, "new": saved})
        pose += 1
if manifest["poses"]:
    with open(os.path.join(OUT, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)
manifest = json.load(open(os.path.join(OUT, "manifest.json")))
both = sum(1 for p in manifest["poses"] if len(p["saved"]) == 2)
print(f"{len(manifest['poses'])} poses ({both} seen by both cameras)")

py = sys.executable
print(subprocess.run([py, "calibrate_intrinsics.py", "--session", OUT, "--out-dir", CAL], capture_output=True, text=True).stdout)
print(subprocess.run([py, "calibrate_stereo.py", "--session", OUT, "--calib-dir", CAL], capture_output=True, text=True).stdout)

print("truth vs fit:")
for serial, (K, dist) in cams.items():
    d = json.load(open(os.path.join(CAL, f"intrinsics_{serial}.json")))
    Kf = np.array(d["camera_matrix"])
    print(f"  {serial}: fx {K[0,0]:.1f}/{Kf[0,0]:.1f} fy {K[1,1]:.1f}/{Kf[1,1]:.1f} "
          f"cx {K[0,2]:.1f}/{Kf[0,2]:.1f} cy {K[1,2]:.1f}/{Kf[1,2]:.1f} "
          f"k1 {dist[0]:.3f}/{d['dist_coeffs'][0]:.3f}")
s = json.load(open(os.path.join(CAL, "stereo_A.json")))
print(f"  top centre true {C_top.tolist()} fit {np.round(s['top_center_in_bottom_frame_mm'], 1).tolist()}")

# pylon_geometry's calibrated cameras vs the true geometry: LEDs 1.5-3 m away.
bottom, top = geom.make_calibrated_pylon_cameras("SYN_A_BOT", "SYN_A_TOP", W, H, calib_dir=CAL)
print(f"  pylon frame: top camera at {np.round(top.origin, 1).tolist()} (true: 0 right, 0 forward, 592 up, "
      f"plus the offsets -> {np.round(geom.CV_TO_LOCAL @ C_top, 1).tolist()})")
errs = []
for _ in range(300):
    X_cv = np.array([rng.uniform(-300, 900), rng.uniform(-500, 500), rng.uniform(1500, 3000)])   # bottom cv frame
    ub, vb = cv2.projectPoints(X_cv.reshape(1, 3), np.zeros(3), np.zeros(3), *cams["SYN_A_BOT"])[0][0, 0]
    Xt = R_true @ X_cv + T_true
    ut, vt = cv2.projectPoints(Xt.reshape(1, 3), np.zeros(3), np.zeros(3), *cams["SYN_A_TOP"])[0][0, 0]
    r = geom.triangulate_pylon_observation(bottom, top, (ub, vb), (ut, vt))
    errs.append(np.linalg.norm(r.point - geom.CV_TO_LOCAL @ X_cv))
errs = np.array(errs)
print(f"  LED triangulation with the fitted calibration: median {np.median(errs):.2f} mm, 95% {np.percentile(errs, 95):.2f} mm")
