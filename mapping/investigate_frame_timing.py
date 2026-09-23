"""
Instruments the raw read() timing to understand the non-monotonic
flush-read-count sensitivity found in test_zero_blob_retry.py: does
cap.read() return quickly (pulling a buffered/backlogged frame) or
slowly (blocking for a genuinely fresh frame, roughly one exposure
period)? And at which read index does the frame content actually
transition from "reflects the old (off) state" to "reflects the new
(lit) state"?

For each trial: fully drain the buffer (many reads) so we start from a
known state, turn off + dwell, then immediately turn the LED on and take
many consecutive reads with NO delay between them (unlike production,
which sleeps dwell_s first) - timestamping each read's wall-clock
duration and classifying its blob content, all from the LED-on command.
This exposes the camera's real buffer depth and frame period, and shows
exactly how many reads/how much elapsed time it actually takes for a
real state change to become visible.

Usage (venv active, run from mapping/):
    python3 investigate_frame_timing.py --top 0 --bottom 2 --name A \\
        --led-list 838 553 --trials 3 --reads 10
"""
import argparse
import time

import neotree_camera as neocam
import neotree_serial as neoser


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
    parser.add_argument('--trials', type=int, default=3, help="repeats per LED")
    parser.add_argument('--reads', type=int, default=10, help="consecutive reads to take per trial, no delay between them")
    parser.add_argument('--drain-reads', type=int, default=10, help="reads to fully empty the buffer before each trial's background capture")
    args = parser.parse_args()

    caps = neocam.initialize_video_capture([args.top, args.bottom])
    if caps is None:
        return
    neocam.set_camera_settings(caps, args.width, args.height, exposure=args.exposure, gain=args.gain)
    cams = {"top": caps[0], "bottom": caps[1]}

    error = neoser.open_neotree_serial('/dev/ttyACM0', baudrate=115200, timeout=0.2)
    if error:
        print(error)
        return
    time.sleep(0.3)
    neoser.ser.read(neoser.ser.in_waiting or 1)

    try:
        for led in args.led_list:
            for trial in range(args.trials):
                neoser.write_tree_all_led(neoser.ser, 3, 0, 0, 0)
                time.sleep(0.3)  # generous settle before draining, not the value under test
                backgrounds = {}
                for pos, cap in cams.items():
                    for _ in range(args.drain_reads):
                        neocam.capture_frame(cap)
                    backgrounds[pos] = neocam.capture_frame(cap)

                print(f"\n--- led={led} trial={trial} ---")
                t_cmd = time.time()
                neoser.write_tree_single_led(neoser.ser, 1, led, 255, 255, 255)

                for pos, cap in cams.items():
                    line = []
                    for i in range(args.reads):
                        t0 = time.time()
                        frame = neocam.capture_frame(cap)
                        t1 = time.time()
                        read_ms = (t1 - t0) * 1000
                        since_cmd_ms = (t1 - t_cmd) * 1000
                        blob_count = 0
                        if frame is not None:
                            _cx, _cy, blob_count, _area = neocam.find_single_blob_centroid(
                                frame, background=backgrounds[pos], threshold_value=args.threshold)
                        status = "OK" if blob_count == 1 else ("ZERO" if blob_count == 0 else "AMB")
                        line.append(f"[{i}] read={read_ms:5.1f}ms t={since_cmd_ms:6.1f}ms {status}")
                    print(f"  {args.name}-{pos}:")
                    for entry in line:
                        print(f"    {entry}")
    finally:
        neoser.write_tree_all_led(neoser.ser, 3, 0, 0, 0)
        neoser.cleanup_serial()
        for cap in caps:
            cap.release()


if __name__ == "__main__":
    main()
