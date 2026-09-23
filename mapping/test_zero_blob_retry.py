"""
Measures whether enabling --retry-on-zero-blobs (off by default in
capture_sweep.py, on the theory that 0 blobs almost always means real
occlusion that a retry can't fix) would actually recover real
detections. diagnose_blobs.py showed the same LED, same physical setup,
flipping between ZERO and OK across identical repeats - contradicting
that theory for at least some LEDs.

For each LED, captures background ONCE (matching production - retries
only re-sample the lit frame, not the background) then takes several
consecutive lit-frame attempts with the real retry_delay between them,
logging every attempt's status instead of stopping at the first
success like production does. From the full per-LED sequences this
computes:
  - baseline: P(attempt 1 == OK) - what happens today with zero-blob
    retry off (single shot, no recovery)
  - recoverable: of the attempt-1 ZEROs, what fraction get an OK on
    attempt 2 or 3 - the real upside retry_on_zero_blobs would add
  - cost: how many extra capture attempts fire in total per LED,
    including on LEDs that never recover (pure overhead - a real
    occlusion doesn't clear no matter how many times you look)

Usage (venv active, run from mapping/):
    python3 test_zero_blob_retry.py --top 0 --bottom 2 --name A \\
        --led-list 426 553 838 597 662 666 836 685 673 589 --attempts 3
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
    parser.add_argument('--gain', type=int, default=255)
    parser.add_argument('--threshold', type=int, default=150)
    parser.add_argument('--led-list', type=int, nargs='+', required=True)
    parser.add_argument('--repeats', type=int, default=3,
                         help="independent trials per LED (each trial captures a fresh background "
                              "and does --attempts consecutive lit reads)")
    parser.add_argument('--attempts', type=int, default=3, help="matches production --retries")
    parser.add_argument('--retry-delay', type=float, default=0.03, help="matches production --retry-delay")
    parser.add_argument('--flush-reads', type=int, default=1,
                         help="debug: how many frames to discard before the 'real' read per attempt "
                              "- production/diagnose_blobs.py use 1; raise this to test whether the "
                              "V4L2 driver's buffer queue needs more than 1 flush to actually catch "
                              "up to the current LED state")
    args = parser.parse_args()

    caps = neocam.initialize_video_capture([args.top, args.bottom])
    if caps is None:
        return
    neocam.set_camera_settings(caps, args.width, args.height, exposure=args.exposure, gain=args.gain)
    cams = {"top": caps[0], "bottom": caps[1]}
    dwell_s = cs.compute_min_dwell_s(args.exposure)

    error = neoser.open_neotree_serial('/dev/ttyACM0', baudrate=115200, timeout=0.2)
    if error:
        print(error)
        return
    time.sleep(0.3)
    neoser.ser.read(neoser.ser.in_waiting or 1)

    # per-camera tally
    tally = {pos: {"attempt1_ok": 0, "attempt1_zero": 0, "recovered": 0,
                    "never_recovered": 0, "extra_attempts_fired": 0, "extra_attempts_wasted": 0,
                    "total_trials": 0}
             for pos in cams}

    try:
        for led in args.led_list:
            for trial in range(args.repeats):
                neoser.write_tree_all_led(neoser.ser, 3, 0, 0, 0)
                time.sleep(dwell_s)
                backgrounds = {}
                for pos, cap in cams.items():
                    neocam.capture_frame(cap)
                    backgrounds[pos] = neocam.capture_frame(cap)

                neoser.write_tree_single_led(neoser.ser, 1, led, 255, 255, 255)
                time.sleep(dwell_s)

                for pos, cap in cams.items():
                    statuses = []
                    for attempt in range(args.attempts):
                        if attempt > 0:
                            time.sleep(args.retry_delay)
                        for _ in range(args.flush_reads):
                            neocam.capture_frame(cap)
                        frame = neocam.capture_frame(cap)
                        blob_count = 0
                        if frame is not None:
                            cx, _cy, blob_count, _area = neocam.find_single_blob_centroid(
                                frame, background=backgrounds[pos], threshold_value=args.threshold)
                        statuses.append("OK" if blob_count == 1 else ("ZERO" if blob_count == 0 else "AMBIG"))

                    t = tally[pos]
                    t["total_trials"] += 1
                    if statuses[0] == "OK":
                        t["attempt1_ok"] += 1
                    elif statuses[0] == "ZERO":
                        t["attempt1_zero"] += 1
                        later_hit = "OK" in statuses[1:]
                        if later_hit:
                            t["recovered"] += 1
                            t["extra_attempts_fired"] += statuses[1:].index("OK") + 1
                        else:
                            t["never_recovered"] += 1
                            t["extra_attempts_wasted"] += len(statuses) - 1
                    print(f"  led={led} trial={trial} {args.name}-{pos}: {statuses}")
    finally:
        neoser.write_tree_all_led(neoser.ser, 3, 0, 0, 0)
        neoser.cleanup_serial()
        for cap in caps:
            cap.release()

    print(f"\n=== Pylon {args.name}: zero-blob retry recovery analysis "
          f"({args.repeats} trials/LED x {len(args.led_list)} LEDs, {args.attempts} attempts/trial) ===")
    for pos, t in tally.items():
        n = t["total_trials"]
        recovery_rate = t["recovered"] / t["attempt1_zero"] if t["attempt1_zero"] else 0.0
        old_ok_rate = t["attempt1_ok"] / n
        new_ok_rate = (t["attempt1_ok"] + t["recovered"]) / n
        print(f"  {args.name}-{pos}: n={n}  attempt1 OK={t['attempt1_ok']} ({old_ok_rate*100:.0f}%) "
              f"ZERO={t['attempt1_zero']}")
        print(f"    of the {t['attempt1_zero']} zeros: {t['recovered']} recovered by a later attempt "
              f"({recovery_rate*100:.0f}% recovery rate), {t['never_recovered']} never recovered")
        print(f"    effective OK-rate with zero-blob retry ON: {new_ok_rate*100:.0f}% "
              f"(vs {old_ok_rate*100:.0f}% today)")
        print(f"    extra capture attempts spent: {t['extra_attempts_fired']} useful (led to a recovery), "
              f"{t['extra_attempts_wasted']} wasted (never recovered anyway)")


if __name__ == "__main__":
    main()
