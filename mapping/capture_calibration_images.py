"""
Captures ChArUco board images for camera calibration - either from a
single camera (per-camera intrinsic calibration: focal length,
principal point, distortion - see calibrate_intrinsics.py) or
simultaneously from a pylon's top+bottom pair (stereo extrinsic
calibration: their true relative pose - see calibrate_stereo.py).

Headless, matching capture_camera_snapshots.py's approach: no live
preview needed. The script detects the ChArUco board in every frame
itself and only saves a pose once it sees a STABLE, good-quality
detection (see the stability-check logic below), printing feedback to
the console instead of requiring a monitor. Hold the board still until
you see "Captured pose N", then move it to a new position/angle/
distance and hold still again for the next one.

Board spec is imported from calibration_board.py - keep that in sync
with the physical board, nothing here should need to change if it is.

Aim for real variety across the captured poses: different distances
(some close for corner density/distortion characterization, some at
the pylons' actual tree-viewing range of ~1-3m), different tilts, and
coverage of every part of the frame (including corners/edges, where
lens distortion is most visible and least constrained by a center-only
capture set). 20+ poses is a reasonable target for single-camera
intrinsics; the same or more for a stereo pair, since a pose only
counts for stereo calibration if BOTH cameras get a good detection of
it simultaneously.

Usage (single camera, for intrinsics):
    python3 capture_calibration_images.py --camera-id 0 \\
        --out-dir calib_images/cam0 --num-poses 20

Usage (stereo pair, for extrinsics - do this AFTER both cameras'
intrinsics are already fit, since stereo calibration reuses them):
    python3 capture_calibration_images.py --camera-id 0 --camera-id-2 2 \\
        --out-dir calib_images/pylonA_stereo --num-poses 20
"""
import argparse
import os
import time

import cv2

import calibration_board as calib
import neotree_camera as neocam


def detect_quality(detector, gray):
    """Returns (num_corners, charuco_ids_set) - charuco_ids_set is used
    for the stability check (same pose should give the same ID set
    frame to frame, not just the same count, which a partial
    occlusion/motion could coincidentally match)."""
    charuco_corners, charuco_ids, _marker_corners, _marker_ids = detector.detectBoard(gray)
    if charuco_ids is None or len(charuco_ids) < calib.MIN_CORNERS_FOR_CAPTURE:
        return 0, frozenset()
    return len(charuco_ids), frozenset(int(i) for i in charuco_ids.flatten())


def wait_for_stable_pose(caps, detector, stable_frames_needed, poll_interval_s):
    """
    Polls all given cameras until EVERY one simultaneously shows a
    good, stable ChArUco detection (same corner-ID set for
    stable_frames_needed consecutive polls) - stability is what
    distinguishes "board is held still, ready to capture" from
    "board is mid-motion, corners flickering in and out." For a
    2-camera (stereo) call, both must be stable AT THE SAME TIME,
    which naturally only happens when the board is genuinely still for
    both viewpoints at once.

    Returns the list of frames (one per cap) at the moment stability
    was confirmed.
    """
    streaks = [0] * len(caps)
    last_ids = [frozenset()] * len(caps)
    last_frames = [None] * len(caps)
    while True:
        all_stable = True
        for idx, cap in enumerate(caps):
            frame = neocam.capture_frame(cap)
            last_frames[idx] = frame
            if frame is None:
                streaks[idx] = 0
                all_stable = False
                continue
            gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
            _n, ids = detect_quality(detector, gray)
            if ids and ids == last_ids[idx]:
                streaks[idx] += 1
            else:
                streaks[idx] = 1 if ids else 0
            last_ids[idx] = ids
            if streaks[idx] < stable_frames_needed:
                all_stable = False
        if all_stable:
            return last_frames
        time.sleep(poll_interval_s)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--camera-id', type=int, required=True)
    parser.add_argument('--camera-id-2', type=int, default=None,
                         help="second camera for a simultaneous stereo pair (e.g. a pylon's "
                              "top+bottom) - omit for single-camera intrinsic capture")
    parser.add_argument('--width', type=int, default=1280)
    parser.add_argument('--height', type=int, default=720)
    parser.add_argument('--exposure', type=int, default=666)
    parser.add_argument('--gain', type=int, default=255)
    parser.add_argument('--fps', type=int, default=30)
    parser.add_argument('--out-dir', required=True)
    parser.add_argument('--num-poses', type=int, default=20)
    parser.add_argument('--stable-frames', type=int, default=5,
                         help="consecutive matching-detection polls required before a pose counts "
                              "as stable enough to capture")
    parser.add_argument('--poll-interval', type=float, default=0.1)
    parser.add_argument('--cooldown-s', type=float, default=2.0,
                         help="pause after each capture, printed as a countdown, so you have time "
                              "to move the board before the next stability check starts")
    args = parser.parse_args()

    camera_ids = [args.camera_id] if args.camera_id_2 is None else [args.camera_id, args.camera_id_2]
    os.makedirs(args.out_dir, exist_ok=True)

    caps = neocam.initialize_video_capture(camera_ids)
    if caps is None:
        return
    neocam.set_camera_settings(caps, args.width, args.height, exposure=args.exposure,
                                gain=args.gain, fps=args.fps)
    neocam.drain_for(caps, 0.5)

    detector = calib.make_detector()
    mode = "stereo pair" if args.camera_id_2 is not None else "single camera"
    print(f"Capturing {args.num_poses} poses ({mode}, cameras {camera_ids}) to {args.out_dir}")
    print(f"Board: {calib.SQUARES_X}x{calib.SQUARES_Y} squares, {calib.CHECKER_MM}mm checker, "
          f"{calib.MARKER_MM}mm marker - min {calib.MIN_CORNERS_FOR_CAPTURE} corners per pose")
    print("Hold the board steady in view - it captures automatically once detection is stable.\n")

    try:
        captured = 0
        while captured < args.num_poses:
            frames = wait_for_stable_pose(caps, detector, args.stable_frames, args.poll_interval)

            counts = []
            ok = True
            for frame in frames:
                gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
                n, _ids = detect_quality(detector, gray)
                counts.append(n)
                if n < calib.MIN_CORNERS_FOR_CAPTURE:
                    ok = False
            if not ok:
                continue

            for idx, (cam_id, frame) in enumerate(zip(camera_ids, frames)):
                path = os.path.join(args.out_dir, f"pose{captured:03d}_cam{cam_id}.jpg")
                cv2.imwrite(path, frame)
            captured += 1
            counts_str = ", ".join(f"cam{cid}={n} corners" for cid, n in zip(camera_ids, counts))
            print(f"Captured pose {captured}/{args.num_poses} ({counts_str})")

            if captured < args.num_poses:
                print(f"  Move the board to a new pose... ({args.cooldown_s:.0f}s)")
                time.sleep(args.cooldown_s)
    finally:
        for cap in caps:
            cap.release()

    print(f"\nDone: {captured} poses saved to {args.out_dir}")


if __name__ == "__main__":
    main()
