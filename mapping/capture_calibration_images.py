"""
Captures ChArUco board images from every camera at once, for both
per-camera intrinsics (calibrate_intrinsics.py: focal length, principal
point, distortion) and each pylon's stereo pair (calibrate_stereo.py: the
top camera's true pose relative to the bottom one) - one session covers
all of it.

Walk the board around the space the cameras see (between the pylons and
the tree, and at the tree itself). A pose is saved whenever at least one
camera sees the board held STILL (the same corners, moving under a pixel,
for several frames running) somewhere it hasn't already captured it; the
frames of every camera that sees it still at that moment are saved
together, so poses seen by both cameras of a pylon feed the stereo fit.

The tree is the status light, so you can watch it instead of a screen:
    dim blue    - no camera sees the board
    amber       - seen; hold still
    purple      - seen and still, but already captured there: move it
    green flash - captured
    white       - every camera has enough poses, all over its view: done
(It uses the Solid mode in the top slot, removed at the end - the scene
underneath is untouched.)

Settings that change a camera's geometry are fixed to what the sweeps use:
the resolution (1280x720 - the sweeps ask for 1208x680 and the driver
gives 1280x720) and manual focus (30). Exposure and gain don't change the
geometry, so they're set for a lit room (auto exposure by default).

Cameras are recorded by their USB serial number, so the calibration
follows the physical camera whatever /dev/video number it gets.

Usage (on the Pi, venv active, from mapping/):
    python3 capture_calibration_images.py --out-dir calib_images/session1
Defaults: cameras 0 2 4 6, pylons A = 0 top / 2 bottom and B = 4 top /
6 bottom (as in the sweeps), 30 poses per camera covering a 3x3 grid of
its view. Ctrl-C stops early; what's captured is kept.
"""
import argparse
import json
import os
import subprocess
import sys
import time

import cv2
import numpy as np

import calibration_board as calib
import neotree_camera as neocam

GRID = 3                  # coverage cells per image axis
STILL_PX = 0.8            # mean corner movement between polls that still counts as still
NEW_POSE_PX = 60.0        # mean corner shift from every earlier capture that makes a pose new


def camera_identity(camera_id):
    """USB serial, model and port of /dev/video<id> (udevadm), for the manifest."""
    info = {"id": camera_id, "serial": f"video{camera_id}", "model": "?", "usb_path": "?"}
    try:
        out = subprocess.run(["udevadm", "info", "-q", "property", "-n", f"/dev/video{camera_id}"],
                             capture_output=True, text=True, timeout=5).stdout
        props = dict(line.split("=", 1) for line in out.splitlines() if "=" in line)
        info["serial"] = props.get("ID_SERIAL_SHORT", info["serial"])
        info["model"] = props.get("ID_V4L_PRODUCT", "?")
        info["usb_path"] = props.get("ID_PATH", "?")
    except (OSError, subprocess.SubprocessError):
        pass
    return info


def detect(detector, frame):
    """{corner id: (x, y)} for a usable detection, else {}."""
    if frame is None:
        return {}
    gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
    corners, ids, _mc, _mi = detector.detectBoard(gray)
    if ids is None or len(ids) < calib.MIN_CORNERS_FOR_CAPTURE:
        return {}
    return {int(i): tuple(c.ravel()) for i, c in zip(ids.flatten(), corners)}


def mean_shift(a, b):
    """Mean movement of the corners two detections share (inf if they share few)."""
    common = set(a) & set(b)
    if not common or len(common) < min(len(a), len(b)) * 0.5:
        return float("inf")
    return float(np.mean([np.hypot(a[i][0] - b[i][0], a[i][1] - b[i][1]) for i in common]))


class CameraState:
    def __init__(self, info, width, height):
        self.info = info
        self.width = width
        self.height = height
        self.last = {}
        self.still_polls = 0
        self.captured = []            # detections saved as new poses
        self.cells = set()
        self.sizes = []

    def update(self, det):
        if not det:
            self.last, self.still_polls = {}, 0
            return
        self.still_polls = self.still_polls + 1 if self.last and mean_shift(det, self.last) < STILL_PX else 0
        self.last = det

    def classify(self, stable_needed):
        """'none', 'moving', 'still-old' (already captured there) or 'still-new'."""
        if not self.last:
            return "none"
        if self.still_polls < stable_needed:
            return "moving"
        if all(mean_shift(self.last, c) > NEW_POSE_PX for c in self.captured):
            return "still-new"
        return "still-old"

    def record(self, det):
        self.captured.append(det)
        pts = np.array(list(det.values()), dtype=np.float32)
        for x, y in pts:
            self.cells.add((min(int(x / self.width * GRID), GRID - 1), min(int(y / self.height * GRID), GRID - 1)))
        self.sizes.append(float(np.sqrt(cv2.contourArea(cv2.convexHull(pts)))))

    def missing_cells(self):
        """Uncovered cells, named as the camera sees the world: the cameras are
        mounted portrait - raw image +x is up, +y the camera's right
        (pylon_geometry.CameraModel.ray_direction)."""
        names = []
        for cx in range(GRID):
            for cy in range(GRID):
                if (cx, cy) not in self.cells:
                    vert = ["low", "middle-height", "high"][cx]
                    horiz = ["to the camera's left", "in the centre", "to the camera's right"][cy]
                    names.append(f"{vert} {horiz}")
        return names

    def done(self, target):
        spread = max(self.sizes) / min(self.sizes) if self.sizes else 0
        return len(self.captured) >= target and len(self.cells) == GRID * GRID and spread >= 1.5


class TreeLight:
    """The tree as a status light: Solid mode in the top slot."""
    COLORS = {"none": (0, 0, 60), "moving": (255, 120, 0), "still-old": (140, 0, 200),
              "capture": (0, 255, 0), "done": (255, 255, 255)}

    def __init__(self, host):
        self.t = None
        self.current = None
        self.slot = 3
        if not host:
            return
        try:
            sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
            import neotree_net as nn
            self.t = nn.connect(host, timeout=5)
            modes = [m["id"] for m in self.t.describe()["modes"]]
            self.t.set_lights(True)
            self.t.set_slot(self.slot, modes.index("solid"), fade=False)
            time.sleep(0.2)
        except Exception as e:   # the light is a nicety: capture works without it
            print(f"(tree status light unavailable: {e})")
            self.t = None

    def show(self, state):
        if self.t is None or state == self.current:
            return
        self.current = state
        try:
            self.t.set_param(self.slot, 0, rgb=self.COLORS[state])
        except Exception as e:
            print(f"(tree status light lost: {e})")
            self.t = None

    def close(self):
        if self.t is not None:
            try:
                self.t.set_slot(self.slot, None, fade=True)
                self.t.close()
            except Exception:
                pass


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--out-dir', required=True)
    parser.add_argument('--cameras', type=int, nargs='+', default=[0, 2, 4, 6])
    parser.add_argument('--pylon', action='append', default=None, metavar="NAME:TOP:BOTTOM",
                        help="a stereo pair by camera id; default A:0:2 and B:4:6 (as in the sweeps)")
    parser.add_argument('--width', type=int, default=1280)
    parser.add_argument('--height', type=int, default=720)
    parser.add_argument('--focus', type=int, default=30, help="manual focus - must match the sweeps")
    parser.add_argument('--exposure', default="auto", help="'auto' or a manual value (V4L2 100us units)")
    parser.add_argument('--gain', type=int, default=64)
    parser.add_argument('--target', type=int, default=30, help="new poses per camera")
    parser.add_argument('--stable-polls', type=int, default=3)
    parser.add_argument('--max-minutes', type=float, default=40)
    parser.add_argument('--tree', default="192.168.0.213", help="the tree, for the status light ('' for none)")
    args = parser.parse_args()

    pylons = args.pylon or ["A:0:2", "B:4:6"]
    os.makedirs(args.out_dir, exist_ok=True)
    infos = [camera_identity(c) for c in args.cameras]
    for info in infos:
        print(f"camera {info['id']}: {info['model']} serial {info['serial']} ({info['usb_path']})")
    by_id = {info["id"]: info for info in infos}

    caps = neocam.initialize_video_capture(args.cameras)
    if caps is None:
        return
    manual = args.exposure != "auto"
    neocam.set_camera_settings(caps, args.width, args.height, exposure=int(args.exposure) if manual else 666,
                               gain=args.gain, focus=args.focus, fps=30)
    if not manual:
        for cap in caps:
            cap.set(cv2.CAP_PROP_AUTO_EXPOSURE, 3)   # aperture priority: exposure follows the room
    neocam.drain_for(caps, 1.0)
    sizes = {(int(c.get(cv2.CAP_PROP_FRAME_WIDTH)), int(c.get(cv2.CAP_PROP_FRAME_HEIGHT))) for c in caps}
    if sizes != {(args.width, args.height)}:
        print(f"ERROR: cameras gave {sizes}, not {args.width}x{args.height} - the calibration wouldn't match")
        for cap in caps:
            cap.release()
        return

    manifest_path = os.path.join(args.out_dir, "manifest.json")
    manifest = {
        "resolution": [args.width, args.height], "focus": args.focus,
        "cameras": infos,
        "pylons": [{"name": n, "top": by_id[int(t)]["serial"], "bottom": by_id[int(b)]["serial"]}
                   for n, t, b in (p.split(":") for p in pylons)],
        "poses": [],
    }

    detector = calib.make_detector()
    states = [CameraState(info, args.width, args.height) for info in infos]
    light = TreeLight(args.tree)
    print(f"\nWalk the board around what the cameras see; hold still to capture. "
          f"Target: {args.target} poses per camera over its whole view.\n")
    t_end = time.time() + args.max_minutes * 60
    try:
        while time.time() < t_end:
            neocam.drain_until_live(caps, max_duration_s=0.3)
            frames = [neocam.capture_frame(c) for c in caps]
            dets = [detect(detector, f) for f in frames]
            for s, d in zip(states, dets):
                s.update(d)
            kinds = [s.classify(args.stable_polls) for s in states]

            if any(k == "still-new" for k in kinds):
                pose = len(manifest["poses"])
                saved, new = [], []
                for s, f, d, k in zip(states, frames, dets, kinds):
                    if k in ("still-new", "still-old"):
                        cv2.imwrite(os.path.join(args.out_dir, f"pose{pose:03d}_{s.info['serial']}.jpg"), f)
                        saved.append(s.info["serial"])
                        if k == "still-new":
                            s.record(d)
                            new.append(s.info["serial"])
                manifest["poses"].append({"pose": pose, "saved": saved, "new": new})
                with open(manifest_path, "w") as fh:
                    json.dump(manifest, fh, indent=2)
                light.show("capture")
                summary = ", ".join(f"cam{s.info['id']} {len(s.captured)}" for s in states)
                print(f"pose {pose}: saved {len(saved)} camera(s) - totals: {summary}", flush=True)
                for s in states:
                    if s.info["serial"] in new and not s.done(args.target):
                        gaps = s.missing_cells()
                        if gaps:
                            print(f"    cam{s.info['id']} still needs: {', '.join(gaps)}", flush=True)
                time.sleep(0.6)
                for s in states:
                    s.still_polls = 0   # the next pose needs a fresh hold
                continue

            if all(s.done(args.target) for s in states):
                light.show("done")
                print("\nEvery camera has its poses. Done.")
                time.sleep(2)
                break
            light.show("still-old" if "still-old" in kinds else "moving" if "moving" in kinds else "none")
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        for cap in caps:
            cap.release()
        light.close()

    print(f"\n{len(manifest['poses'])} poses saved in {args.out_dir}")
    for s in states:
        spread = max(s.sizes) / min(s.sizes) if s.sizes else 0
        print(f"  cam{s.info['id']} ({s.info['serial']}): {len(s.captured)} poses, "
              f"{len(s.cells)}/{GRID * GRID} cells, near/far size ratio {spread:.1f}")


if __name__ == "__main__":
    main()
