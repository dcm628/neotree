"""
Runs one mapping sweep: walks LEDs one at a time, captures a centroid from
each active pylon's top/bottom cameras, triangulates a 3D position (in that
pylon's own local frame) per LED per pylon, and logs everything to
sweep_data.sqlite3 (see sweep_db.py for the schema/raw-vs-derived split).

Camera-to-pylon wiring isn't assumed fixed across sessions (USB
enumeration order can shift) - confirm ids with identify_cameras.py first,
then pass them explicitly. At least one pylon (both --pylon-*-top and
--pylon-*-bottom) is required; a second is optional.

No cross-sweep/cross-pylon alignment happens here - each pylon's solves
stay in its own local frame. That registration step is a separate,
later phase; this script's job ends at "here's what one pylon's stereo
pair could see this sweep."

Usage (venv active):
    python3 capture_sweep.py --pylon-a-top 0 --pylon-a-bottom 2 \\
        --pylon-b-top 4 --pylon-b-bottom 6 --notes "first full sweep, tree front"
"""
import argparse
import time

import cv2

import neotree_camera as neocam
import neotree_serial as neoser
import pylon_geometry as geom
import sweep_db


def build_pylon(pylon_id, top_id, bottom_id, width, height, spacing_mm):
    captures = neocam.initialize_video_capture([top_id, bottom_id])
    if captures is None:
        raise RuntimeError(f"pylon {pylon_id}: could not open camera ids {top_id}/{bottom_id}")
    neocam.set_camera_settings(captures, width=width, height=height)
    top_cap, bottom_cap = captures
    # The driver doesn't always honor the requested resolution (seen in
    # practice: 1208x680 requested, 1280x720 negotiated) - the geometry
    # model's focal length must match what's actually coming back, or
    # every triangulation is silently off.
    actual_width = int(top_cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    actual_height = int(top_cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    if actual_width != width or actual_height != height:
        print(f"pylon {pylon_id}: requested {width}x{height}, driver negotiated "
              f"{actual_width}x{actual_height} - using the actual size for triangulation")
    bottom_model, top_model = geom.make_pylon_cameras(actual_width, actual_height, spacing_mm=spacing_mm)
    return {
        "pylon_id": pylon_id,
        "top_id": top_id, "bottom_id": bottom_id,
        "top_cap": top_cap, "bottom_cap": bottom_cap,
        "top_model": top_model, "bottom_model": bottom_model,
        "top_bg": None, "bottom_bg": None,
    }


def capture_reference_frames(pylon):
    """One all-LEDs-off reference frame per camera, for background subtraction."""
    # discard one buffered frame first - the capture object can otherwise
    # hand back a frame queued before the LED state actually changed
    neocam.capture_frame(pylon["top_cap"])
    neocam.capture_frame(pylon["bottom_cap"])
    pylon["top_bg"] = neocam.capture_frame(pylon["top_cap"])
    pylon["bottom_bg"] = neocam.capture_frame(pylon["bottom_cap"])


def capture_centroid_with_retry(cap, background, dwell_s, retries, threshold_value):
    """Fixed dwell (already elapsed by the caller before the first call) + retry."""
    for attempt in range(1, retries + 1):
        neocam.capture_frame(cap)  # flush a possibly-stale buffered frame
        frame = neocam.capture_frame(cap)
        if frame is not None:
            cx, cy, blob_count, blob_area = neocam.find_single_blob_centroid(
                frame, background=background, threshold_value=threshold_value)
            if cx is not None:
                return cx, cy, blob_count, blob_area, attempt
        if attempt < retries:
            time.sleep(dwell_s / 2)
    return None, None, blob_count if frame is not None else 0, blob_area if frame is not None else 0.0, retries


def run_sweep(conn, sweep_id, pylons, led_count, dwell_s, retries, pixel_sigma_px, threshold_value):
    for i in range(led_count):
        neoser.write_tree_all_led(neoser.ser, 3, 0, 0, 0)
        neoser.write_tree_single_led(neoser.ser, 1, i, 255, 255, 255)
        time.sleep(dwell_s)

        for pylon in pylons:
            obs = {}
            for pos, cap, model, bg in (
                ("top", pylon["top_cap"], pylon["top_model"], pylon["top_bg"]),
                ("bottom", pylon["bottom_cap"], pylon["bottom_model"], pylon["bottom_bg"]),
            ):
                cx, cy, blob_count, blob_area, attempts = capture_centroid_with_retry(
                    cap, bg, dwell_s, retries, threshold_value)
                found = cx is not None
                sweep_db.record_raw_observation(
                    conn, sweep_id, pylon["pylon_id"], pos, i, found,
                    pixel_x=cx, pixel_y=cy, blob_count=blob_count,
                    blob_area=blob_area, attempts=attempts)
                if found:
                    obs[pos] = (cx, cy)

            if "top" in obs and "bottom" in obs:
                result = geom.triangulate_pylon_observation(
                    pylon["bottom_model"], pylon["top_model"],
                    obs["bottom"], obs["top"], pixel_sigma_px=pixel_sigma_px)
                sweep_db.record_session_solve(conn, sweep_id, pylon["pylon_id"], i, result)

        if (i + 1) % 100 == 0:
            print(f"  {i + 1}/{led_count} LEDs walked")


def print_summary(conn, sweep_id, led_count):
    summary = sweep_db.sweep_summary(conn, sweep_id)
    print(f"\n--- sweep {sweep_id} summary ({led_count} LEDs walked) ---")
    for pylon_id, counts in summary.items():
        print(f"  pylon {pylon_id}: solved={counts['solved']} "
              f"partial(1 camera only)={counts['partial_one_camera']} "
              f"no_centroid={counts['no_centroid']}")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--pylon-a-top', type=int)
    parser.add_argument('--pylon-a-bottom', type=int)
    parser.add_argument('--pylon-b-top', type=int)
    parser.add_argument('--pylon-b-bottom', type=int)
    parser.add_argument('--db', default=sweep_db.DEFAULT_DB_PATH)
    parser.add_argument('--width', type=int, default=1208)
    parser.add_argument('--height', type=int, default=680)
    parser.add_argument('--spacing-mm', type=float, default=geom.PYLON_CAMERA_SPACING_MM,
                         help="vertical distance between each pylon's top/bottom camera")
    parser.add_argument('--count', type=int, default=1000)
    parser.add_argument('--dwell', type=float, default=0.2, help="seconds to wait after lighting an LED")
    parser.add_argument('--retries', type=int, default=3)
    parser.add_argument('--threshold', type=int, default=250)
    parser.add_argument('--pixel-sigma', type=float, default=1.0,
                         help="assumed 1-sigma centroid pixel noise, for the covariance estimate")
    parser.add_argument('--notes', default=None)
    args = parser.parse_args()

    pylon_args = {
        "A": (args.pylon_a_top, args.pylon_a_bottom),
        "B": (args.pylon_b_top, args.pylon_b_bottom),
    }
    requested = {pid: ids for pid, ids in pylon_args.items() if ids[0] is not None or ids[1] is not None}
    for pid, (top_id, bottom_id) in requested.items():
        if top_id is None or bottom_id is None:
            parser.error(f"pylon {pid}: both --pylon-{pid.lower()}-top and "
                         f"--pylon-{pid.lower()}-bottom are required if either is given")
    if not requested:
        parser.error("at least one pylon (--pylon-a-top/--pylon-a-bottom) is required")

    error = neoser.open_neotree_serial('/dev/ttyACM0', baudrate=115200)
    if error:
        print(error)
        return
    neoser.write_tree_all_led(neoser.ser, 3, 0, 0, 0)
    time.sleep(0.25)

    pylons = []
    try:
        for pid, (top_id, bottom_id) in requested.items():
            print(f"Opening pylon {pid}: top={top_id} bottom={bottom_id}")
            pylons.append(build_pylon(pid, top_id, bottom_id, args.width, args.height, args.spacing_mm))

        print("Capturing all-off reference frames for background subtraction...")
        for pylon in pylons:
            capture_reference_frames(pylon)

        conn = sweep_db.connect(args.db)
        try:
            while True:
                sweep_id = sweep_db.start_sweep(conn, notes=args.notes)
                for pylon in pylons:
                    sweep_db.record_pylon_placement(
                        conn, sweep_id, pylon["pylon_id"], pylon["top_id"], pylon["bottom_id"],
                        args.width, args.height, args.spacing_mm, calibration_source="nominal")

                print(f"\nStarting sweep {sweep_id} ({args.count} LEDs, "
                      f"dwell={args.dwell}s, retries={args.retries})...")
                t0 = time.time()
                run_sweep(conn, sweep_id, pylons, args.count, args.dwell, args.retries,
                          args.pixel_sigma, args.threshold)
                print(f"Sweep {sweep_id} done in {time.time() - t0:.1f}s")
                print_summary(conn, sweep_id, args.count)

                answer = input("\nRelocate pylon(s) and run another sweep? [y/N]: ").strip().lower()
                if answer != 'y':
                    break
                input("Reposition camera(s) now, then press Enter to start the next sweep...")
                for pylon in pylons:
                    capture_reference_frames(pylon)
        finally:
            conn.close()
    finally:
        neoser.write_tree_all_led(neoser.ser, 3, 0, 0, 0)
        for pylon in pylons:
            pylon["top_cap"].release()
            pylon["bottom_cap"].release()
        neoser.cleanup_serial()


if __name__ == "__main__":
    main()
