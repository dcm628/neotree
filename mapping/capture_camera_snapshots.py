"""
Headless replacement for identify_cameras.py's live preview: captures one
still frame from each given camera ID and saves it to disk, instead of
requiring a DISPLAY connection to a physical monitor plugged into the Pi
(identify_cameras.py's X11 preview windows only work that way, which was
the one thing forcing the Pi to stay non-headless).

Lets aim/focus/visibility be checked remotely - pull the saved files back
over scp (or hand them to Claude to inspect via its file-reading tools)
instead of needing eyes on the Pi's own monitor.

Lights the whole tree white by default before capturing - in a fully
dark room (no ambient light at all), a snapshot with every LED off has
nothing for the camera to see regardless of aim, which isn't useful for
checking where a pylon is actually pointed. Pass --no-light-tree to skip
this (e.g. checking aim under real ambient light instead).

Usage (venv active, run from mapping/):
    python3 capture_camera_snapshots.py --ids 0 2 4 6 --out-dir /tmp/cam_snapshots
    python3 capture_camera_snapshots.py --ids 0 2 --label pylon_a_recheck
"""
import argparse
import os
import time

import cv2

import neotree_camera as neocam
import neotree_serial as neoser


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--ids', type=int, nargs='+', default=[0, 2, 4, 6])
    parser.add_argument('--width', type=int, default=1208)
    parser.add_argument('--height', type=int, default=680)
    parser.add_argument('--exposure', type=int, default=666)
    parser.add_argument('--gain', type=int, default=255)
    parser.add_argument('--fps', type=int, default=30)
    parser.add_argument('--settle-s', type=float, default=1.0,
                         help="how long to drain/settle each camera before the real snapshot")
    parser.add_argument('--out-dir', default='/tmp/cam_snapshots')
    parser.add_argument('--label', default=None,
                         help="optional tag included in each filename, e.g. to distinguish repeated checks")
    parser.add_argument('--light-tree', action=argparse.BooleanOptionalAction, default=True,
                         help="turn every LED on white before capturing (default: on) - without an "
                              "active light source, a fully dark room gives every camera a blank "
                              "frame regardless of aim")
    parser.add_argument('--light-settle-s', type=float, default=0.5)
    args = parser.parse_args()

    os.makedirs(args.out_dir, exist_ok=True)
    timestamp = time.strftime('%Y%m%d_%H%M%S')
    tag = f"_{args.label}" if args.label else ""

    if args.light_tree:
        error = neoser.open_neotree_serial('/dev/ttyACM0', baudrate=115200, timeout=0.2)
        if error:
            print(error)
            return
        time.sleep(0.3)
        neoser.ser.read(neoser.ser.in_waiting or 1)
        neoser.write_tree_all_led(neoser.ser, 3, 255, 255, 255)
        time.sleep(args.light_settle_s)

    saved = []
    for cam_id in args.ids:
        caps = neocam.initialize_video_capture([cam_id])
        if caps is None:
            print(f"camera {cam_id}: failed to open, skipping")
            continue
        cap = caps[0]
        neocam.set_camera_settings(caps, args.width, args.height, exposure=args.exposure,
                                    gain=args.gain, fps=args.fps)
        neocam.drain_for(caps, args.settle_s)
        frame = neocam.capture_frame(cap)
        cap.release()

        if frame is None:
            print(f"camera {cam_id}: failed to capture a frame")
            continue

        path = os.path.join(args.out_dir, f"cam{cam_id}{tag}_{timestamp}.jpg")
        cv2.imwrite(path, frame)
        saved.append(path)
        print(f"camera {cam_id}: saved {path} ({frame.shape[1]}x{frame.shape[0]})")

    if args.light_tree:
        neoser.write_tree_all_led(neoser.ser, 3, 0, 0, 0)
        neoser.cleanup_serial()

    print(f"\n{len(saved)}/{len(args.ids)} snapshots saved to {args.out_dir}")


if __name__ == "__main__":
    main()
