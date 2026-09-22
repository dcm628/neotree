"""
Shows a live, ID-labeled preview of each given camera so the physical
top/bottom-per-pylon wiring can be confirmed by eye before a sweep. USB
enumeration order isn't guaranteed stable across boots/replugs, so this
should be re-run (or at least re-checked) each session rather than assumed
from a fixed camera_id list.

Usage (venv active, needs a live display - DISPLAY defaults to :0, see
treeImageCaptureTest.py for why):
    python3 identify_cameras.py --ids 0 2 4 6
Press 'q' to quit.
"""
import argparse
import os

os.environ.setdefault("DISPLAY", ":0")

import cv2

import neotree_camera as neocam


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--ids', type=int, nargs='+', required=True,
                         help="candidate /dev/video* ids to preview")
    args = parser.parse_args()

    captures = neocam.initialize_video_capture(args.ids)
    if captures is None:
        print("Error: one or more cameras could not be opened.")
        return
    neocam.set_camera_settings(captures, width=1208, height=680)

    print("Showing live preview for each camera id - press 'q' to quit.")
    try:
        while True:
            for camera_id, cap in zip(args.ids, captures):
                frame = neocam.capture_frame(cap)
                if frame is None:
                    continue
                display = neocam.process_for_display_frame(frame)
                cv2.putText(display, f"id={camera_id}", (10, 30),
                            cv2.FONT_HERSHEY_SIMPLEX, 1.0, (0, 255, 0), 2)
                cv2.imshow(f"camera {camera_id}", display)
            if cv2.waitKey(1) & 0xFF == ord('q'):
                break
    finally:
        for cap in captures:
            cap.release()
        cv2.destroyAllWindows()


if __name__ == "__main__":
    main()
