"""
Large-sample, retry-free, noise-free A/B comparison of the blob-fragmentation
fix (close_kernel_size in neotree_camera.find_single_blob_centroid): for
each LED position, captures exactly ONE background/lit frame pair (no
retries - retry_on_ambiguous would otherwise re-sample and mask exactly
the effect being measured), then evaluates that SAME frame through the
detector twice - once with close_kernel_size=0 (old behavior) and once
with the current default (fixed behavior). Because both evaluations run
on identical pixel data, there is zero live-hardware/signal-drift noise
between the two conditions - any difference is attributable purely to
the code change.

Usage (venv active, run from mapping/):
    python3 compare_blob_fix.py --top 0 --bottom 2 --name A \\
        --start-led 0 --led-span 1000 --stride 5
"""
import argparse
import time

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
    parser.add_argument('--threshold', type=int, default=150)
    parser.add_argument('--start-led', type=int, default=0)
    parser.add_argument('--led-span', type=int, default=1000)
    parser.add_argument('--stride', type=int, default=5, help="sample every Nth LED across the span")
    parser.add_argument('--dwell', type=float, default=None)
    parser.add_argument('--new-min-area', type=int, default=20,
                         help="min_area used for the 'new' (close_kernel_size=3) condition only - "
                              "raising this compensates for closing occasionally growing a "
                              "previously-sub-threshold noise speck just past the old min_area, "
                              "which showed up as a small number of real regressions in testing. "
                              "The 'old' (close=0) condition always uses the original default (20) "
                              "as the true baseline.")
    args = parser.parse_args()

    caps = neocam.initialize_video_capture([args.top, args.bottom])
    if caps is None:
        return
    neocam.set_camera_settings(caps, args.width, args.height, exposure=args.exposure)
    top_cap, bottom_cap = caps
    cams = {"top": top_cap, "bottom": bottom_cap}

    dwell_s = args.dwell if args.dwell is not None else cs.compute_min_dwell_s(args.exposure)
    led_positions = list(range(args.start_led, args.start_led + args.led_span, args.stride))
    print(f"Comparing on {len(led_positions)} LED positions (stride={args.stride}), dwell={dwell_s*1000:.1f}ms, "
          f"threshold={args.threshold}, single capture per position (no retries)")

    error = neoser.open_neotree_serial('/dev/ttyACM0', baudrate=115200, timeout=0.2)
    if error:
        print(error)
        return
    time.sleep(0.3)
    neoser.ser.read(neoser.ser.in_waiting or 1)

    # old_status/new_status counts, per camera position (top/bottom)
    tally = {pos: {"old": {"OK": 0, "ZERO": 0, "AMBIG": 0}, "new": {"OK": 0, "ZERO": 0, "AMBIG": 0}}
             for pos in cams}
    changed = {pos: 0 for pos in cams}
    fixed_cases = {pos: [] for pos in cams}  # old==AMBIG, new==OK
    regressions = {pos: [] for pos in cams}  # old==OK, new!=OK (should never happen)

    try:
        for i, led in enumerate(led_positions, start=1):
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

                _cx, _cy, old_count, _area = neocam.find_single_blob_centroid(
                    lit, background=backgrounds[pos], threshold_value=args.threshold, close_kernel_size=0)
                _cx2, _cy2, new_count, _area2 = neocam.find_single_blob_centroid(
                    lit, background=backgrounds[pos], threshold_value=args.threshold, close_kernel_size=3,
                    min_area=args.new_min_area)

                old_status = "OK" if old_count == 1 else ("ZERO" if old_count == 0 else "AMBIG")
                new_status = "OK" if new_count == 1 else ("ZERO" if new_count == 0 else "AMBIG")
                tally[pos]["old"][old_status] += 1
                tally[pos]["new"][new_status] += 1
                if old_status != new_status:
                    changed[pos] += 1
                    if old_status == "AMBIG" and new_status == "OK":
                        fixed_cases[pos].append(led)
                    elif old_status == "OK" and new_status != "OK":
                        regressions[pos].append(led)

            if i % 40 == 0 or i == len(led_positions):
                print(f"  {i}/{len(led_positions)} positions tested (led={led})")
    finally:
        neoser.write_tree_all_led(neoser.ser, 3, 0, 0, 0)
        neoser.cleanup_serial()
        for cap in caps:
            cap.release()

    print(f"\n=== Pylon {args.name}: old (close=0) vs new (close=3) over {len(led_positions)} positions ===")
    for pos in cams:
        o, n = tally[pos]["old"], tally[pos]["new"]
        print(f"  {args.name}-{pos}: old OK={o['OK']} ZERO={o['ZERO']} AMBIG={o['AMBIG']}  |  "
              f"new OK={n['OK']} ZERO={n['ZERO']} AMBIG={n['AMBIG']}  |  "
              f"changed={changed[pos]} (fixed AMBIG->OK={len(fixed_cases[pos])}, "
              f"regressions OK->other={len(regressions[pos])})")
        if fixed_cases[pos]:
            print(f"    fixed at leds: {fixed_cases[pos]}")
        if regressions[pos]:
            print(f"    REGRESSIONS at leds: {regressions[pos]}")


if __name__ == "__main__":
    main()
