"""
Diagnostic tool: for a set of LED positions, captures the exact
background/lit frame pair a real sweep would use (same dwell, same
flush-then-read pattern as capture_sweep.py), runs the same single-blob
detector, and for every non-single-blob result saves the raw background
frame, raw lit frame, the background-subtracted+thresholded mask, and an
annotated copy with detected blob outlines - so the actual cause (a
reflection, ambient light drift between the two captures, JPEG
compression noise, genuine occlusion) can be inspected visually instead
of guessed at from blob-count statistics alone.

Usage (venv active, run from mapping/):
    python3 diagnose_blobs.py --top 0 --bottom 2 --name A \\
        --led-list 426 553 838 597 662 --out-dir /tmp/blob_diag
"""
import argparse
import os
import time

import cv2

import neotree_camera as neocam
import neotree_serial as neoser
import capture_sweep as cs


def save_diag(out_dir, tag, background, lit, threshold_value):
    os.makedirs(out_dir, exist_ok=True)
    gray_bg = cv2.cvtColor(background, cv2.COLOR_BGR2GRAY)
    gray_lit = cv2.cvtColor(lit, cv2.COLOR_BGR2GRAY)
    diff = cv2.subtract(gray_lit, gray_bg)
    _, mask = cv2.threshold(diff, threshold_value, 255, cv2.THRESH_BINARY)
    num_labels, labels, stats, centroids = cv2.connectedComponentsWithStats(mask, connectivity=8)

    annotated = lit.copy()
    for i in range(1, num_labels):
        area = stats[i, cv2.CC_STAT_AREA]
        if area < 20:
            continue
        x, y, w, h = stats[i, cv2.CC_STAT_LEFT], stats[i, cv2.CC_STAT_TOP], \
            stats[i, cv2.CC_STAT_WIDTH], stats[i, cv2.CC_STAT_HEIGHT]
        cx, cy = centroids[i]
        cv2.rectangle(annotated, (x, y), (x + w, y + h), (0, 0, 255), 2)
        cv2.putText(annotated, f"a={int(area)}", (x, max(0, y - 5)),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 0, 255), 1)

    cv2.imwrite(os.path.join(out_dir, f"{tag}_background.jpg"), background)
    cv2.imwrite(os.path.join(out_dir, f"{tag}_lit.jpg"), lit)
    cv2.imwrite(os.path.join(out_dir, f"{tag}_mask.jpg"), mask)
    cv2.imwrite(os.path.join(out_dir, f"{tag}_annotated.jpg"), annotated)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--top', type=int, required=True)
    parser.add_argument('--bottom', type=int, required=True)
    parser.add_argument('--name', default='A')
    parser.add_argument('--width', type=int, default=1208)
    parser.add_argument('--height', type=int, default=680)
    parser.add_argument('--exposure', type=int, default=666)
    parser.add_argument('--threshold', type=int, default=150)
    parser.add_argument('--led-list', type=int, nargs='+', required=True)
    parser.add_argument('--dwell', type=float, default=None,
                         help="default: compute_min_dwell_s's current production value")
    parser.add_argument('--repeats', type=int, default=3,
                         help="repeat each LED this many times - reflections/occlusion should "
                              "reproduce every time, transient noise won't")
    parser.add_argument('--out-dir', default='/tmp/blob_diag')
    parser.add_argument('--save-all', action='store_true',
                         help="save every trial's frames, not just ambiguous/failed ones")
    args = parser.parse_args()

    caps = neocam.initialize_video_capture([args.top, args.bottom])
    if caps is None:
        return
    neocam.set_camera_settings(caps, args.width, args.height, exposure=args.exposure)
    top_cap, bottom_cap = caps
    cams = {"top": top_cap, "bottom": bottom_cap}

    dwell_s = args.dwell if args.dwell is not None else cs.compute_min_dwell_s(args.exposure)
    print(f"Using dwell={dwell_s*1000:.1f}ms, threshold={args.threshold}")

    error = neoser.open_neotree_serial('/dev/ttyACM0', baudrate=115200, timeout=0.2)
    if error:
        print(error)
        return
    time.sleep(0.3)
    neoser.ser.read(neoser.ser.in_waiting or 1)

    saved = 0
    try:
        for led in args.led_list:
            for rep in range(args.repeats):
                neoser.write_tree_all_led(neoser.ser, 3, 0, 0, 0)
                time.sleep(dwell_s)
                backgrounds = {}
                for pos, cap in cams.items():
                    neocam.capture_frame(cap)
                    backgrounds[pos] = neocam.capture_frame(cap)

                neoser.write_tree_single_led(neoser.ser, 1, led, 255, 255, 255)
                time.sleep(dwell_s)

                for pos, cap in cams.items():
                    neocam.capture_frame(cap)
                    lit = neocam.capture_frame(cap)
                    if lit is None or backgrounds[pos] is None:
                        continue
                    cx, cy, blob_count, area = neocam.find_single_blob_centroid(
                        lit, background=backgrounds[pos], threshold_value=args.threshold)
                    status = "OK" if blob_count == 1 else ("ZERO" if blob_count == 0 else "AMBIG")
                    print(f"  led={led} rep={rep} {args.name}-{pos}: {status} "
                          f"blob_count={blob_count} area={area:.0f}")
                    if args.save_all or blob_count != 1:
                        tag = f"led{led}_rep{rep}_{args.name}-{pos}_{status}"
                        save_diag(args.out_dir, tag, backgrounds[pos], lit, args.threshold)
                        saved += 1
    finally:
        neoser.write_tree_all_led(neoser.ser, 3, 0, 0, 0)
        neoser.cleanup_serial()
        for cap in caps:
            cap.release()

    print(f"\nSaved {saved} diagnostic image sets to {args.out_dir}")


if __name__ == "__main__":
    main()
