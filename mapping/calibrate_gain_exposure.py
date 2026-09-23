"""
Staged (exposure, gain) calibration for one pylon's cameras, exploiting
full-dark conditions where ambient light isn't forcing gain to stay
maxed. gain=255 (current default) is flagged in neotree_camera.py's own
docstring as likely adding real sensor noise on top of being stable -
this tests whether it can be backed off now, and whether exposure can
also shift, without losing detection reliability.

Metric: OK-classification rate (single clean blob, not zero, not
fragmented/ambiguous) across repeated captures of LEDs already known
visible from this pylon (low-residual solves from an earlier real
sweep) - targets stability at positions that should produce a clean
signal, not raw sensitivity to marginal ones.

Coordinate-descent search, not a full grid (would take too long):
  1. Fix exposure at --base-exposure, sweep --gain-candidates.
  2. At the best gain from stage 1, sweep --exposure-candidates.
  3. Report the winning (exposure, gain), its OK-rate, its blob-area
     stability (mean/stdev - a real noise reduction should tighten
     this), and the resulting compute_min_dwell_s (since exposure
     directly sets the dwell floor - a calibration that only "wins" by
     inflating exposure/dwell isn't free).

Usage (venv active, run from mapping/):
    python3 calibrate_gain_exposure.py --top 0 --bottom 2 --name A \\
        --led-list 426 553 838 597 662 666 836 685 673 589
"""
import argparse
import statistics
import time

import cv2

import neotree_camera as neocam
import neotree_serial as neoser
import capture_sweep as cs


def capture_once(cams, led, dwell_s, threshold_value):
    # Drains each camera's buffer throughout the dwell instead of a blind
    # sleep + single flush read - a single flush was confirmed (see
    # investigate_frame_timing.py / neotree_camera.drain_for) to often
    # still land on a stale, pre-transition frame.
    all_caps = list(cams.values())
    neoser.write_tree_all_led(neoser.ser, 3, 0, 0, 0)
    neocam.drain_for(all_caps, dwell_s)
    backgrounds = {}
    for pos, cap in cams.items():
        backgrounds[pos] = neocam.capture_frame(cap)

    neoser.write_tree_single_led(neoser.ser, 1, led, 255, 255, 255)
    neocam.drain_for(all_caps, dwell_s)

    results = {}
    for pos, cap in cams.items():
        lit = neocam.capture_frame(cap)
        if lit is None or backgrounds[pos] is None:
            results[pos] = (0, 0.0)
            continue
        _cx, _cy, blob_count, area = neocam.find_single_blob_centroid(
            lit, background=backgrounds[pos], threshold_value=threshold_value)
        results[pos] = (blob_count, area)
    return results


def evaluate_setting(cams, led_list, repeats, dwell_s, threshold_value):
    stats = {pos: {"ok": 0, "zero": 0, "ambig": 0, "areas": []} for pos in cams}
    total = len(led_list) * repeats
    for led in led_list:
        for _ in range(repeats):
            results = capture_once(cams, led, dwell_s, threshold_value)
            for pos, (blob_count, area) in results.items():
                if blob_count == 1:
                    stats[pos]["ok"] += 1
                    stats[pos]["areas"].append(area)
                elif blob_count == 0:
                    stats[pos]["zero"] += 1
                else:
                    stats[pos]["ambig"] += 1
    summary = {}
    for pos, s in stats.items():
        areas = s["areas"]
        summary[pos] = {
            "ok_rate": s["ok"] / total,
            "zero": s["zero"], "ambig": s["ambig"], "ok": s["ok"],
            "area_mean": statistics.mean(areas) if areas else 0.0,
            "area_stdev": statistics.stdev(areas) if len(areas) > 1 else 0.0,
        }
    return summary


def apply_settings(caps, top_id, bottom_id, width, height, exposure, gain, focus, fps):
    """
    Fully closes and reopens both cameras for every settings change,
    rather than reusing the same already-open, already-streaming capture
    objects and just re-calling .set() on them. Confirmed by testing:
    reconfiguring a live-streaming device's exposure/gain repeatedly
    doesn't reliably take hold (readback kept drifting/snapping back to
    a stuck value across candidates, e.g. every requested exposure
    reading back as the same ~312 regardless), while a fresh open +
    single configure (exactly what build_pylon does for a real sweep)
    was confirmed stable and accurate. Slower - a real device reopen per
    candidate - but this is calibration, not the sweep itself.
    """
    for cap in caps:
        cap.release()
    new_caps = neocam.initialize_video_capture([top_id, bottom_id])
    neocam.set_camera_settings(new_caps, width=width, height=height, exposure=exposure, gain=gain,
                                focus=focus, fps=fps)
    time.sleep(0.3)
    neocam.drain_for(new_caps, 0.3)
    actual_exposure = new_caps[0].get(cv2.CAP_PROP_EXPOSURE)
    actual_gain = new_caps[0].get(cv2.CAP_PROP_GAIN)
    return new_caps, actual_exposure, actual_gain


def print_row(label, dwell_s, summary):
    parts = [f"{label:>22}  dwell={dwell_s*1000:5.1f}ms"]
    for pos, s in summary.items():
        parts.append(f"{pos}: OK={s['ok_rate']*100:4.0f}% (n={s['ok']}) "
                      f"zero={s['zero']} ambig={s['ambig']} "
                      f"area={s['area_mean']:.0f}+/-{s['area_stdev']:.0f}")
    print("  ".join(parts))


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--top', type=int, required=True)
    parser.add_argument('--bottom', type=int, required=True)
    parser.add_argument('--name', default='A')
    parser.add_argument('--width', type=int, default=1208)
    parser.add_argument('--height', type=int, default=680)
    parser.add_argument('--threshold', type=int, default=150)
    parser.add_argument('--focus', type=int, default=30)
    parser.add_argument('--led-list', type=int, nargs='+', required=True)
    parser.add_argument('--validate-led-list', type=int, nargs='+', default=None,
                         help="a second, disjoint LED set to validate the winning setting against "
                              "- if omitted, skips stage 3 validation")
    parser.add_argument('--repeats', type=int, default=4)
    parser.add_argument('--fps', type=int, default=10,
                         help="camera frame rate - caps the max real exposure (a UVC camera "
                              "silently clamps exposure to fit 1/fps regardless of what's "
                              "requested, confirmed by testing). All --exposure-candidates should "
                              "stay comfortably under 1000/fps ms or they'll get clamped too.")
    parser.add_argument('--base-exposure', type=int, default=666)
    parser.add_argument('--gain-candidates', type=int, nargs='+', default=[255, 192, 128, 64, 0])
    parser.add_argument('--exposure-candidates', type=int, nargs='+',
                         default=[200, 400, 666, 900])
    args = parser.parse_args()

    caps = neocam.initialize_video_capture([args.top, args.bottom])
    if caps is None:
        return
    cams = {"top": caps[0], "bottom": caps[1]}

    error = neoser.open_neotree_serial('/dev/ttyACM0', baudrate=115200, timeout=0.2)
    if error:
        print(error)
        return
    time.sleep(0.3)
    neoser.ser.read(neoser.ser.in_waiting or 1)

    best_gain = None
    best_exposure = None
    try:
        print(f"=== Stage 1: exposure fixed at {args.base_exposure}, sweeping gain (fps={args.fps}) ===")
        stage1_results = []
        for gain in args.gain_candidates:
            caps, actual_exp, actual_gain = apply_settings(
                caps, args.top, args.bottom, args.width, args.height, args.base_exposure, gain,
                args.focus, args.fps)
            cams = {"top": caps[0], "bottom": caps[1]}
            dwell_s = cs.compute_min_dwell_s(actual_exp, args.fps)
            summary = evaluate_setting(cams, args.led_list, args.repeats, dwell_s, args.threshold)
            print_row(f"gain={actual_gain:.0f}", dwell_s, summary)
            avg_ok = statistics.mean(s["ok_rate"] for s in summary.values())
            stage1_results.append((gain, avg_ok, summary))

        best_gain = max(stage1_results, key=lambda r: r[1])[0]
        print(f"\nBest gain from stage 1: {best_gain} (highest average OK-rate)")

        print(f"\n=== Stage 2: gain fixed at {best_gain}, sweeping exposure (fps={args.fps}) ===")
        stage2_results = []
        for exposure in args.exposure_candidates:
            caps, actual_exp, actual_gain = apply_settings(
                caps, args.top, args.bottom, args.width, args.height, exposure, best_gain,
                args.focus, args.fps)
            cams = {"top": caps[0], "bottom": caps[1]}
            dwell_s = cs.compute_min_dwell_s(actual_exp, args.fps)
            summary = evaluate_setting(cams, args.led_list, args.repeats, dwell_s, args.threshold)
            print_row(f"exposure={actual_exp:.0f}", dwell_s, summary)
            avg_ok = statistics.mean(s["ok_rate"] for s in summary.values())
            stage2_results.append((actual_exp, avg_ok, dwell_s, summary))

        best_exposure, best_avg_ok, best_dwell, best_summary = max(stage2_results, key=lambda r: r[1])
        print(f"\nBest exposure from stage 2: {best_exposure:.0f} (avg OK-rate {best_avg_ok*100:.0f}%, "
              f"dwell={best_dwell*1000:.1f}ms)")

        if args.validate_led_list:
            print(f"\n=== Stage 3: validating exposure={best_exposure:.0f} gain={best_gain} "
                  f"against a fresh LED set ===")
            caps, actual_exp, _actual_gain = apply_settings(
                caps, args.top, args.bottom, args.width, args.height, best_exposure, best_gain,
                args.focus, args.fps)
            cams = {"top": caps[0], "bottom": caps[1]}
            dwell_s = cs.compute_min_dwell_s(actual_exp, args.fps)
            summary = evaluate_setting(cams, args.validate_led_list, args.repeats, dwell_s, args.threshold)
            print_row("validation", dwell_s, summary)

            print(f"\n=== Baseline comparison: original exposure={args.base_exposure} gain=255, "
                  f"same validation set, fps={args.fps} ===")
            caps, actual_exp_base, _actual_gain_base = apply_settings(
                caps, args.top, args.bottom, args.width, args.height, args.base_exposure, 255,
                args.focus, args.fps)
            cams = {"top": caps[0], "bottom": caps[1]}
            dwell_s_base = cs.compute_min_dwell_s(actual_exp_base, args.fps)
            summary_base = evaluate_setting(cams, args.validate_led_list, args.repeats, dwell_s_base, args.threshold)
            print_row("baseline", dwell_s_base, summary_base)
    finally:
        neoser.write_tree_all_led(neoser.ser, 3, 0, 0, 0)
        neoser.cleanup_serial()
        for cap in caps:
            cap.release()

    print(f"\nRecommendation: exposure={best_exposure:.0f} gain={best_gain} fps={args.fps}")


if __name__ == "__main__":
    main()
