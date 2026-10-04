"""
Two checks with the ChArUco board before a calibration session
(docs/CALIBRATION.md, "Phased mapping"):

focus - hold (or prop) the board still at the working distance, facing the
        cameras; it steps every camera's manual focus through a range and
        measures how sharp the board is at each, then suggests the setting
        to use for that phase. Each camera's intrinsics are then calibrated
        at that focus (capture_calibration_images.py --focus).

range - walk the board slowly away from the cameras; it reports each
        camera's detection and the board's distance, and remembers the
        furthest each one still reads it. Use it to check the board is
        detected across the working distances (the phase 1 plan needs up
        to ~1.25 m).

The tree is the status light: amber while measuring; in range mode green
when every camera reads the board, amber when some do, dim blue for none.

Usage (on the Pi, venv active, from mapping/):
    python3 board_check.py focus --distance-mm 1100
    python3 board_check.py range --focus 60
"""
import argparse
import time

import cv2
import numpy as np

import calibration_board as calib
import neotree_camera as neocam
import pylon_geometry as geom
from capture_calibration_images import TreeLight


def board_view(detector, board, frame):
    """(corner count, object points, image points, hull mask) of the board in a frame."""
    gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
    corners, ids, _mc, _mi = detector.detectBoard(gray)
    if ids is None or len(ids) < 6:
        return 0, None, None, None, gray
    obj, img = board.matchImagePoints(corners, ids)
    mask = np.zeros(gray.shape, np.uint8)
    cv2.fillConvexPoly(mask, cv2.convexHull(img.reshape(-1, 2).astype(np.int32)), 255)
    return len(ids), obj, img, mask, gray


def sharpness(gray, mask):
    """Gradient energy inside the board over its contrast: independent of lighting."""
    gx = cv2.Sobel(gray, cv2.CV_32F, 1, 0, ksize=3)
    gy = cv2.Sobel(gray, cv2.CV_32F, 0, 1, ksize=3)
    m = mask > 0
    contrast = float(np.std(gray[m])) + 1e-6
    return float(np.mean(gx[m] ** 2 + gy[m] ** 2)) / (contrast ** 2)


def open_cameras(ids, width, height, focus):
    caps = neocam.initialize_video_capture(ids)
    if caps is None:
        raise SystemExit("couldn't open the cameras")
    neocam.set_camera_settings(caps, width, height, exposure=666, gain=64, focus=focus, fps=30)
    for cap in caps:
        cap.set(cv2.CAP_PROP_AUTO_EXPOSURE, 3)
    neocam.drain_for(caps, 1.0)
    return caps


def focus_mode(args):
    infos = [neocam.camera_identity(c) for c in args.cameras]
    caps = open_cameras(args.cameras, args.width, args.height, args.start)
    board, detector = calib.make_board(), calib.make_detector()
    light = TreeLight(args.tree)
    light.show("moving")
    print(f"Hold the board still about {args.distance_mm} mm from the cameras, facing them.\n")
    results = {info["serial"]: [] for info in infos}
    try:
        for focus in range(args.start, args.stop + 1, args.step):
            for cap in caps:
                cap.set(cv2.CAP_PROP_FOCUS, focus)
            neocam.drain_for(caps, 0.6)                 # the lens motor settles
            neocam.drain_until_live(caps, max_duration_s=0.3)
            line = [f"focus {focus:3d}:"]
            for info, cap in zip(infos, caps):
                best = None
                for _ in range(2):
                    frame = neocam.capture_frame(cap)
                    n, _o, _i, mask, gray = board_view(detector, board, frame) if frame is not None else (0,) * 5
                    if n:
                        s = sharpness(gray, mask)
                        best = s if best is None else max(best, s)
                results[info["serial"]].append((focus, best))
                line.append(f"cam{info['id']} {'-' if best is None else f'{best:6.2f}'}")
            print("  ".join(line), flush=True)
    finally:
        for cap in caps:
            cap.release()
        light.show("done")
        time.sleep(1)
        light.close()

    print("\nsharpest focus per camera:")
    scores = {}
    for info in infos:
        rows = [(f, s) for f, s in results[info["serial"]] if s is not None]
        if not rows:
            print(f"  cam{info['id']} ({info['serial']}): the board was never detected")
            continue
        top = max(s for _f, s in rows)
        best_f = max(rows, key=lambda r: r[1])[0]
        print(f"  cam{info['id']} ({info['serial']}): {best_f}")
        for f, s in rows:
            scores.setdefault(f, []).append(s / top)
    # One setting for all: the best worst-camera sharpness.
    common = [(min(v), f) for f, v in scores.items() if len(v) == len(infos)]
    if common:
        worst, f = max(common)
        print(f"\nsuggested focus for every camera: {f} (each camera at least {worst:.0%} of its own sharpest)")


def range_mode(args):
    infos = [neocam.camera_identity(c) for c in args.cameras]
    caps = open_cameras(args.cameras, args.width, args.height, args.focus)
    board, detector = calib.make_board(), calib.make_detector()
    Ks = []
    for info in infos:
        d = geom.load_intrinsics(info["serial"], args.calib_dir, args.focus)
        if d is not None:
            Ks.append((np.array(d["camera_matrix"]), np.array(d["dist_coeffs"])))
        else:   # nominal: good enough for a distance readout
            Ks.append((np.array([[940.0, 0, args.width / 2], [0, 940.0, args.height / 2], [0, 0, 1]]), np.zeros(5)))
    light = TreeLight(args.tree)
    furthest = {info["serial"]: 0.0 for info in infos}
    print("Walk the board slowly away from the cameras, facing them. Ctrl-C to finish.\n")
    try:
        last_print = 0.0
        while True:
            neocam.drain_until_live(caps, max_duration_s=0.3)
            line, seen = [], 0
            for info, cap, (K, D) in zip(infos, caps, Ks):
                frame = neocam.capture_frame(cap)
                n, obj, img, _m, _g = board_view(detector, board, frame) if frame is not None else (0,) * 5
                dist = None
                if n >= calib.MIN_CORNERS_FOR_CAPTURE:
                    ok, _r, t = cv2.solvePnP(obj, img, K, D)
                    if ok:
                        dist = float(np.linalg.norm(t)) * 1000.0
                        furthest[info["serial"]] = max(furthest[info["serial"]], dist)
                        seen += 1
                line.append(f"cam{info['id']} {n:3d} corners" + (f" {dist / 1000:.2f} m" if dist else "      "))
            light.show("capture" if seen == len(infos) else "moving" if seen else "none")
            if time.time() - last_print > 0.5:
                print("  ".join(line), flush=True)
                last_print = time.time()
    except KeyboardInterrupt:
        pass
    finally:
        for cap in caps:
            cap.release()
        light.close()
    print("\nfurthest distance with a usable detection (30+ corners):")
    for info in infos:
        print(f"  cam{info['id']} ({info['serial']}): {furthest[info['serial']] / 1000:.2f} m")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="mode", required=True)
    for name in ("focus", "range"):
        p = sub.add_parser(name)
        p.add_argument('--cameras', type=int, nargs='+', default=[0, 2, 4, 6])
        p.add_argument('--width', type=int, default=1280)
        p.add_argument('--height', type=int, default=720)
        p.add_argument('--tree', default="192.168.0.213", help="the tree, for the status light ('' for none)")
    f = sub.choices["focus"]
    f.add_argument('--distance-mm', type=int, default=1100, help="where the board is held (for the message)")
    f.add_argument('--start', type=int, default=0)
    f.add_argument('--stop', type=int, default=150)
    f.add_argument('--step', type=int, default=5)
    r = sub.choices["range"]
    r.add_argument('--focus', type=int, required=True)
    r.add_argument('--calib-dir', default=geom.DEFAULT_CALIBRATION_DIR)
    args = parser.parse_args()
    focus_mode(args) if args.mode == "focus" else range_mode(args)


if __name__ == "__main__":
    main()
