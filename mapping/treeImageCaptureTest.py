import os
import warnings

# Set environment variable to suppress warnings globally
os.environ["PYTHONWARNINGS"] = "ignore:.*iCCP.*:UserWarning"

# cv2.imshow() needs a live X/XWayland display. A terminal on the Pi's own
# desktop already has this (DISPLAY=:0), but a plain SSH session (e.g. a
# VSCode Remote-SSH terminal) does not, which makes the Qt platform plugin
# fail with "could not connect to display". Default to the Pi's own local
# session; only set if DISPLAY isn't already present so this is a no-op when
# it's already correct (running locally, or via ssh -X/-Y forwarding).
os.environ.setdefault("DISPLAY", ":0")

import cv2
import numpy as np
import time
import neotree_serial
import neotree_camera as neocam
import neotree_serial as neoser


def main():
    # --- Open the serial port to the Pico ---
    error = neoser.open_neotree_serial('/dev/ttyACM0', baudrate=115200)
    if error:
        print(error)
        return  # can't do anything without the serial link

    # Turn all LEDs off to start (msg_type 3 = all LEDs)
    neoser.write_tree_all_led(neotree_serial.ser, 3, 0, 0, 0)
    time.sleep(.25)
    if neotree_serial.ser.in_waiting > 0:  # drain any reply from the Pico
        neotree_serial.ser.readline().decode('utf-8').strip()
    time.sleep(.25)

    # --- Single-LED walk settings (msg_type 1 = single LED) ---
    msg_type = 1
    r, g, b = 255, 255, 255      # full white
    max_led_position = 500

    # --- Initialize the cameras ---
    # 4 USB webcams exposed as V4L2 device ids (two top/bottom pairs).
    camera_ids = [0, 2, 4, 6]
    captures = neocam.initialize_video_capture(camera_ids)
    if captures is None:
        print("Error: one or more cameras could not be opened. "
              "Check which /dev/video* ids are present and adjust camera_ids.")
        neoser.cleanup_serial()
        return

    # Set frame size and exposure for all cameras
    neocam.set_camera_settings(captures, width=1208, height=680, exposure=-5)

    # --- Main capture loop ---
    frame_count = 0
    while True:
        for idx, cap in enumerate(captures):
            frame = neocam.capture_frame(cap)
            if frame is None:
                continue  # skip if this camera didn't return a frame

            # Center of mass of the bright (lit-LED) region
            center = neocam.find_center_of_mass(frame, threshold_value=250)
            if center:
                cx, cy = center
                cv2.circle(frame, (cx, cy), 10, (0, 255, 0), -1)
                cv2.putText(frame, f"Center: ({cx}, {cy})", (cx + 10, cy - 10),
                            cv2.FONT_HERSHEY_SIMPLEX, 0.5, (255, 255, 255), 2)
                print(f"cam {idx} center x: {cx} y: {cy}")

            display_frame = neocam.process_for_display_frame(frame)
            cv2.imshow(f"Camera {idx} - Center of Mass", display_frame)

        # Press 'q' in any window to quit
        key = cv2.waitKey(1) & 0xFF
        if key == ord('q'):
            break

        frame_count += 1
        print(f"Captured frame {frame_count}")

        # Advance the single lit LED: all off, then light the next position white
        neoser.write_tree_all_led(neotree_serial.ser, 3, 0, 0, 0)
        time.sleep(.125)
        led_position = frame_count % max_led_position
        neoser.write_tree_single_led(neotree_serial.ser, msg_type, led_position, r, g, b)

    # --- Cleanup ---
    for cap in captures:
        cap.release()
    cv2.destroyAllWindows()
    neoser.cleanup_serial()


if __name__ == "__main__":
    main()
