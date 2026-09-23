"""
Empirically measures whether compute_min_dwell_s's frame_margin_s term
(capture_sweep.py, currently a guessed 25ms, never measured) is actually
enough headroom - by testing detection reliability directly at a range of
candidate dwell values, rather than trying to time individual sub-stages.

Replicates the exact real capture pattern used in a sweep
(capture_background_frames / capture_centroid_with_retry in
capture_sweep.py): light an LED, sleep the candidate dwell, then do one
flush read (discarding a possibly-stale buffered frame) followed by one
real read - with retries disabled, since a retry's extra retry_delay_s
sleep would mask an insufficient dwell by effectively giving it more
elapsed time, which isn't what we're trying to measure here.

For each candidate dwell, runs several trials per active camera and
reports the single-attempt success rate (single blob correctly detected
against a background captured with the same candidate dwell). The
current production default (LED_REFRESH_MARGIN_S + exposure + 25ms) is
included in the candidate list by default so its result can be read
directly off the same table as everything else.

Usage (venv active, run from mapping/):
    python3 measure_dwell_timing.py --pylon-a-top 0 --pylon-a-bottom 2 \\
        --pylon-b-top 4 --pylon-b-bottom 6 --trials-per-dwell 15
"""
import argparse
import random
import time

import neotree_camera as neocam
import neotree_serial as neoser
import capture_sweep as cs


def run_trial(cams, led, dwell_s, threshold_value):
    """One trial across all cams: off -> dwell -> flush+read (background),
    on -> dwell -> flush+read (lit). Returns {name: hit_bool}."""
    neoser.write_tree_all_led(neoser.ser, 3, 0, 0, 0)
    time.sleep(dwell_s)
    backgrounds = {}
    for name, cap in cams.items():
        neocam.capture_frame(cap)  # flush, matches capture_background_frames
        backgrounds[name] = neocam.capture_frame(cap)

    neoser.write_tree_single_led(neoser.ser, 1, led, 255, 255, 255)
    time.sleep(dwell_s)

    hits = {}
    for name, cap in cams.items():
        neocam.capture_frame(cap)  # flush, matches capture_centroid_with_retry
        frame = neocam.capture_frame(cap)
        cx = cy = None
        blob_count = 0
        if frame is not None:
            cx, cy, blob_count, _area = neocam.find_single_blob_centroid(
                frame, background=backgrounds[name], threshold_value=threshold_value)
        hits[name] = (cx is not None, blob_count)
    return hits


def screen_led_pool(cams, led_pool, dwell_s, threshold_value, repeats, want):
    """
    Finds LED positions reliably visible (single clean blob, every repeat)
    to EVERY active camera at a generous dwell - isolating physical
    visibility (occlusion, distance, reflection noise at that specific
    string position) from timing sensitivity. Without this, a random LED
    pool mixes "camera can't see this position at all" failures in with
    "dwell was too short" failures, and the two are indistinguishable in
    the success-rate numbers.
    """
    good = []
    random.shuffle(led_pool)
    for led in led_pool:
        if len(good) >= want:
            break
        ok = True
        for _ in range(repeats):
            hits = run_trial(cams, led, dwell_s, threshold_value)
            if not all(hit for hit, _count in hits.values()):
                ok = False
                break
        print(f"  screening led={led}: {'PASS' if ok else 'fail'}")
        if ok:
            good.append(led)
    return good


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--pylon-a-top', type=int)
    parser.add_argument('--pylon-a-bottom', type=int)
    parser.add_argument('--pylon-b-top', type=int)
    parser.add_argument('--pylon-b-bottom', type=int)
    parser.add_argument('--width', type=int, default=1208)
    parser.add_argument('--height', type=int, default=680)
    parser.add_argument('--exposure', type=int, default=666)
    parser.add_argument('--threshold', type=int, default=150)
    parser.add_argument('--start-led', type=int, default=650)
    parser.add_argument('--led-span', type=int, default=100)
    parser.add_argument('--trials-per-dwell', type=int, default=15)
    parser.add_argument('--dwell-candidates-ms', type=float, nargs='+', default=None,
                         help="default: [30, 50, 70, 90, current_auto, current_auto+30, "
                              "current_auto+60] where current_auto is compute_min_dwell_s's "
                              "existing value at --exposure")
    parser.add_argument('--screen', action='store_true',
                         help="pre-screen led_pool for positions visible to ALL active cameras "
                              "at a generous dwell, and use only that clean set for the timed "
                              "trials - removes per-position visibility noise from the timing "
                              "signal (see screen_led_pool)")
    parser.add_argument('--screen-dwell-ms', type=float, default=300.0)
    parser.add_argument('--screen-repeats', type=int, default=2)
    parser.add_argument('--screen-want', type=int, default=12)
    parser.add_argument('--led-list', type=int, nargs='+', default=None,
                         help="use this explicit, fixed set of LED positions instead of "
                              "--start-led/--led-span (and skip --screen entirely) - e.g. "
                              "positions already known-solved with low residual from an earlier "
                              "sweep with the pylons in the same physical location, which is "
                              "stronger evidence of real visibility than a blind screening pass")
    args = parser.parse_args()

    ids = {'A-top': args.pylon_a_top, 'A-bottom': args.pylon_a_bottom,
           'B-top': args.pylon_b_top, 'B-bottom': args.pylon_b_bottom}
    ids = {k: v for k, v in ids.items() if v is not None}
    if not ids:
        parser.error("at least one camera id is required")

    caps = neocam.initialize_video_capture(list(ids.values()))
    if caps is None:
        return
    neocam.set_camera_settings(caps, args.width, args.height, exposure=args.exposure)
    cams = dict(zip(ids.keys(), caps))

    auto_dwell_ms = cs.compute_min_dwell_s(args.exposure) * 1000
    if args.dwell_candidates_ms is None:
        candidates_ms = sorted(set(
            [30, 50, 70, 90, round(auto_dwell_ms), round(auto_dwell_ms + 30), round(auto_dwell_ms + 60)]))
    else:
        candidates_ms = sorted(args.dwell_candidates_ms)

    print(f"compute_min_dwell_s({args.exposure}) = {auto_dwell_ms:.1f}ms (current production default)")
    print(f"Testing candidate dwells (ms): {candidates_ms}")

    error = neoser.open_neotree_serial('/dev/ttyACM0', baudrate=115200, timeout=0.2)
    if error:
        print(error)
        return
    time.sleep(0.3)
    neoser.ser.read(neoser.ser.in_waiting or 1)

    led_pool = list(range(args.start_led, args.start_led + args.led_span))
    fixed_pool = False

    if args.led_list is not None:
        led_pool = list(args.led_list)
        fixed_pool = True
        print(f"Using explicit fixed LED list ({len(led_pool)} positions): {led_pool}")
    elif args.screen:
        print(f"\n=== Screening {len(led_pool)} candidate LEDs at {args.screen_dwell_ms:.0f}ms dwell "
              f"({args.screen_repeats} repeats, need all cams to hit every repeat) ===")
        good_leds = screen_led_pool(cams, led_pool, args.screen_dwell_ms / 1000.0, args.threshold,
                                     args.screen_repeats, args.screen_want)
        print(f"Screened set: {len(good_leds)}/{args.screen_want} wanted -> {good_leds}")
        if len(good_leds) < 3:
            print("Too few clean positions found - can't get a meaningful timing signal here.")
            neoser.write_tree_all_led(neoser.ser, 3, 0, 0, 0)
            neoser.cleanup_serial()
            for cap in caps:
                cap.release()
            return
        led_pool = good_leds
        fixed_pool = True

    results = {name: {} for name in cams}
    ambiguous_counts = {name: {} for name in cams}
    try:
        for dwell_ms in candidates_ms:
            dwell_s = dwell_ms / 1000.0
            print(f"\n=== dwell={dwell_ms:.0f}ms ({args.trials_per_dwell} trials) ===")
            hit_counts = {name: 0 for name in cams}
            ambig_counts = {name: 0 for name in cams}
            for trial in range(args.trials_per_dwell):
                led = led_pool[trial % len(led_pool)] if fixed_pool else random.choice(led_pool)
                hits = run_trial(cams, led, dwell_s, args.threshold)
                for name, (hit, blob_count) in hits.items():
                    if hit:
                        hit_counts[name] += 1
                    elif blob_count > 1:
                        ambig_counts[name] += 1
            for name in cams:
                rate = hit_counts[name] / args.trials_per_dwell
                results[name][dwell_ms] = rate
                ambiguous_counts[name][dwell_ms] = ambig_counts[name]
                print(f"  {name}: {hit_counts[name]}/{args.trials_per_dwell} detected "
                      f"({rate*100:.0f}%), {ambig_counts[name]} ambiguous(>1 blob)")
    finally:
        neoser.write_tree_all_led(neoser.ser, 3, 0, 0, 0)
        neoser.cleanup_serial()
        for cap in caps:
            cap.release()

    print("\n=== Summary: detection success rate vs dwell ===")
    header = "dwell(ms)".ljust(10) + "".join(name.ljust(12) for name in cams)
    print(header)
    for dwell_ms in candidates_ms:
        row = f"{dwell_ms:.0f}".ljust(10)
        for name in cams:
            row += f"{results[name][dwell_ms]*100:.0f}%".ljust(12)
        marker = "  <- current default" if round(dwell_ms) == round(auto_dwell_ms) else ""
        print(row + marker)


if __name__ == "__main__":
    main()
