"""
Validates the drain_for fix (neotree_camera.py) against the exact same
kind of repeated-capture consistency check that first exposed the
flakiness (diagnose_blobs.py): the same LED, same physical setup,
captured repeatedly, should get a CONSISTENT status if the underlying
signal is real and stable - flip-flopping between ZERO/OK/AMBIG across
identical repeats was the original clue that something upstream of the
LED's real visibility was noisy. This uses capture_sweep.py's actual
production capture pattern (capture_background_frames +
capture_centroid_with_retry, single attempt) rather than a
reimplementation, so it's a direct measure of the real fix, not a
proxy.

Usage (venv active, run from mapping/):
    python3 validate_drain_fix.py --top 0 --bottom 2 --name A \\
        --led-list 426 553 838 597 662 666 836 685 673 589 --repeats 4
"""
import argparse
import time

import cv2

import neotree_camera as neocam
import neotree_serial as neoser
import capture_sweep as cs


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--top', type=int, required=True)
    parser.add_argument('--bottom', type=int, required=True)
    parser.add_argument('--name', default='A')
    parser.add_argument('--width', type=int, default=1208)
    parser.add_argument('--height', type=int, default=680)
    parser.add_argument('--exposure', type=int, default=666)
    parser.add_argument('--gain', type=int, default=255)
    parser.add_argument('--fps', type=int, default=10,
                         help="caps the max real exposure - a UVC camera silently clamps exposure "
                              "to fit 1/fps regardless of what's requested (confirmed by testing)")
    parser.add_argument('--threshold', type=int, default=150)
    parser.add_argument('--led-list', type=int, nargs='+', required=True)
    parser.add_argument('--repeats', type=int, default=4)
    args = parser.parse_args()

    caps = neocam.initialize_video_capture([args.top, args.bottom])
    if caps is None:
        return
    neocam.set_camera_settings(caps, args.width, args.height, exposure=args.exposure, gain=args.gain,
                                fps=args.fps)
    neocam.drain_for(caps, 0.3)  # settle + confirm the real exposure before trusting the readback
    actual_exposure = caps[0].get(cv2.CAP_PROP_EXPOSURE)
    pylon = {"pylon_id": args.name, "top_cap": caps[0], "bottom_cap": caps[1]}
    all_caps = [caps[0], caps[1]]
    dwell_s = cs.compute_min_dwell_s(actual_exposure, args.fps)
    print(f"requested exposure={args.exposure} actual={actual_exposure} dwell={dwell_s*1000:.1f}ms "
          f"threshold={args.threshold}")

    error = neoser.open_neotree_serial('/dev/ttyACM0', baudrate=115200, timeout=0.2)
    if error:
        print(error)
        return
    time.sleep(0.3)
    neoser.ser.read(neoser.ser.in_waiting or 1)

    consistency = {"top": [], "bottom": []}
    ok_counts = {"top": 0, "bottom": 0}
    total = {"top": 0, "bottom": 0}
    try:
        for led in args.led_list:
            sequences = {"top": [], "bottom": []}
            for trial in range(args.repeats):
                neoser.write_tree_all_led(neoser.ser, 3, 0, 0, 0)
                neocam.drain_for(all_caps, dwell_s)
                top_bg, bottom_bg = cs.capture_background_frames(pylon)

                neoser.write_tree_single_led(neoser.ser, 1, led, 255, 255, 255)
                neocam.drain_for(all_caps, dwell_s)

                for pos, cap, bg in (("top", caps[0], top_bg), ("bottom", caps[1], bottom_bg)):
                    frame = neocam.capture_frame(cap)
                    blob_count = 0
                    if frame is not None:
                        _cx, _cy, blob_count, _area = neocam.find_single_blob_centroid(
                            frame, background=bg, threshold_value=args.threshold)
                    status = "OK" if blob_count == 1 else ("ZERO" if blob_count == 0 else "AMBIG")
                    sequences[pos].append(status)
                    total[pos] += 1
                    if status == "OK":
                        ok_counts[pos] += 1

            for pos in ("top", "bottom"):
                seq = sequences[pos]
                is_consistent = len(set(seq)) == 1
                consistency[pos].append(is_consistent)
                marker = "" if is_consistent else "  <-- INCONSISTENT"
                print(f"  led={led} {args.name}-{pos}: {seq}{marker}")
    finally:
        neoser.write_tree_all_led(neoser.ser, 3, 0, 0, 0)
        neoser.cleanup_serial()
        for cap in caps:
            cap.release()

    print(f"\n=== Pylon {args.name}: drain-fix validation ({args.repeats} repeats x {len(args.led_list)} LEDs) ===")
    for pos in ("top", "bottom"):
        n_consistent = sum(consistency[pos])
        n_leds = len(consistency[pos])
        ok_rate = ok_counts[pos] / total[pos]
        print(f"  {args.name}-{pos}: {n_consistent}/{n_leds} LEDs gave a fully consistent status "
              f"across all {args.repeats} repeats ({n_consistent/n_leds*100:.0f}%), "
              f"overall OK-rate={ok_rate*100:.0f}%")


if __name__ == "__main__":
    main()
