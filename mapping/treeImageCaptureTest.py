import os
import warnings

# Set environment variable to suppress warnings globally
os.environ["PYTHONWARNINGS"] = "ignore:.*iCCP.*:UserWarning"

import cv2
import numpy as np
import time
import neotree_serial
import neotree_camera as neocam
import neotree_serial as neoser

# from neotree_serial import open_neotree_serial, write_neotree_serial, write_tree_single_led, write_tree_all_led, cleanup_serial


    


def main():
    # Open the serial port
    error = neoser.open_neotree_serial('/dev/ttyACM0', baudrate=115200)
    if error:
        print(error)

    # Send the data for full led off command
        msg_type = 3  # Using msg_type = 3
        r, g, b = 0, 0, 0  # RGB values (0, 0, 0)    
        neoser.write_tree_all_led(neotree_serial.ser, msg_type, r, g, b)
#         print(neotree_serial.ser.in_waiting)
        time.sleep(.25)
        if neotree_serial.ser.in_waiting > 0:  # Check if there is any data to read
#             print(neotree_serial.ser.in_waiting)
            line = neotree_serial.ser.readline().decode('utf-8').strip()  # Read the line and decode it
            
#             print(f"Received from Pico: {line}")        
        time.sleep(.25)
        
    msg_type = 1  # Using msg_type = 1 for single LED command   
    r, g, b = 255, 255, 255  # RGB values to max
    current_position = 0
    max_led_position = 500
    
    # Loop from 1 to 500
#     for led_position in range(0, 500):
#         msg_type = 1  # Using msg_type = 1
#         r, g, b = 0, 25, 0  # RGB values (0, 0, 0)
    
        # Send the data for the current LED position
#         write_tree_single_led(neotree_serial.ser, msg_type, led_position, r, g, b)
#         print(neotree_serial.ser.in_waiting)
#         if neotree_serial.ser.in_waiting > 0:  # Check if there is any data to read
#             print(neotree_serial.ser.in_waiting)
#             line = neotree_serial.ser.readline().decode('utf-8').strip()  # Read the line and decode it
            
#             print(f"Received from Pico: {line}")        
#         time.sleep(.25)
    
    
    # List of camera IDs (0, 1, ..., n)
#     camera_ids = [0, 2, 4, 6]  # For now, we have two cameras, but you can add more IDs later.
    # Create a list to hold pylon objects
    pylons = []

    # Dynamically add pylons
    pylons.append(CameraPylon(camera_id_top=0, camera_id_bottom=2, z_offset=100, coordinates_top=(10, 20, 0), coordinates_bottom=(10, 20, 100)))
    pylons.append(CameraPylon(camera_id_top=4, camera_id_bottom=6, z_offset=100, coordinates_top=(15, 25, 0), coordinates_bottom=(15, 25, 100)))

    # Additional pylons can be added as needed
    # pylons.append(CameraPylon(camera_id_top=8, camera_id_bottom=10, z_offset=100, coordinates_top=(20, 30, 0), coordinates_bottom=(20, 30, 100)))

    # Initialize the video capture objects for each camera
#     captures = neocam.initialize_video_capture(camera_ids)

#     if captures is None:
#         return  # Exit if any camera could not be initialized

    # Set the camera settings (frame size and exposure) for all cameras
#     neocam.set_camera_settings(captures, width=1920, height=1280, exposure=-4)
    neocam.set_camera_settings(captures, width=1208, height=680, exposure=-5)

    # Loop to continuously capture and process frames for each camera
    frame_count = 0
    while True:
        for idx, cap in enumerate(captures):
            # Capture frame from the current camera
            frame = neocam.capture_frame(cap)
            
            if frame is None:
                continue  # Skip this iteration if the frame is not captured

            # Find the center of mass (centroid) of the thresholded image
            center = neocam.find_center_of_mass(frame, threshold_value=250)

            if center:
                cx, cy = center
                # Draw the center of mass on the frame
                cv2.circle(frame, (cx, cy), 10, (0, 255, 0), -1)  # Green circle at the center of mass

                # Display the coordinates of the center
                cv2.putText(frame, f"Center: ({cx}, {cy})", (cx + 10, cy - 10),
                            cv2.FONT_HERSHEY_SIMPLEX, 0.5, (255, 255, 255), 2)
                print(f"center x: {cx} y: {cy}")
            cv2.waitKey(25)
#             display_frame = cv2.resize(frame,(360,540))
            display_frame = neocam.process_for_display_frame(frame)
            # Display the processed frame
            cv2.imshow(f"Camera {idx} - Center of Mass", display_frame)
#             time.sleep(.1)

        # Optionally add a short delay to control the frame rate
        key = cv2.waitKey(1) & 0xFF
        if key == ord('q'):  # Press 'q' to quit
            break

        frame_count += 1
        print(f"Captured frame {frame_count}")
        
        neoser.write_tree_all_led(neotree_serial.ser, 3, 0, 0, 0)
#         time.sleep(.005)       
        time.sleep(.125)  
        # Send the data for the current LED position
        led_position = frame_count % max_led_position
        neoser.write_tree_single_led(neotree_serial.ser, msg_type, led_position, r, g, b)
        print(neotree_serial.ser.in_waiting)

    # Release the video capture objects when done
    for cap in captures:
        cap.release()

    # Clean up and close the OpenCV windows
    cv2.destroyAllWindows()
    
    clean_up_serial()

if __name__ == "__main__":
    main()