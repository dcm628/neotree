import os
import warnings

# Set environment variable to suppress warnings globally
os.environ["PYTHONWARNINGS"] = "ignore:.*iCCP.*:UserWarning"

import cv2
import numpy as np
import time

def initialize_video_capture(camera_ids):
    """
    Initialize the video capture objects for multiple cameras and check if each is available.

    :param camera_ids: List of camera IDs (e.g., [0, 1] for two cameras).
    :return: List of video capture objects if successful, None if any camera fails to initialize.
    """
    captures = []
    for camera_id in camera_ids:
        cap = cv2.VideoCapture(camera_id, cv2.CAP_V4L2)  # Use V4L2 backend

        if not cap.isOpened():
            print(f"Error: Could not open camera {camera_id}.")
            return None
        else:
            print(f"Success: Opened camera {camera_id}.")
            
        captures.append(cap)
    return captures

def set_camera_settings(captures, width=1920, height=1080, exposure=-5):
    """
    Set various camera settings for all cameras in the list.

    :param captures: List of video capture objects.
    :param width: Desired frame width (default is 1920).
    :param height: Desired frame height (default is 1080).
    :param exposure: Exposure value (default is -5).
    :return: None
    """
    for cap in captures:
        cap.set(cv2.CAP_PROP_FRAME_WIDTH, width)
        cap.set(cv2.CAP_PROP_FRAME_HEIGHT, height)
        cap.set(cv2.CAP_PROP_EXPOSURE, exposure)

    # Print the current settings to verify (optional)
    for idx, cap in enumerate(captures):
        actual_width = cap.get(cv2.CAP_PROP_FRAME_WIDTH)
        actual_height = cap.get(cv2.CAP_PROP_FRAME_HEIGHT)
        actual_exposure = cap.get(cv2.CAP_PROP_EXPOSURE)
        print(f"Camera {idx}: Width={actual_width}, Height={actual_height}, Exposure={actual_exposure}")

def capture_frame(cap):
    """
    Capture a single frame from the webcam.

    :param cap: The video capture object.
    :return: The captured frame as a numpy array or None if there's an error.
    """
    ret, frame = cap.read()

    if not ret:
        print("Error: Failed to capture image.")
        return None

    return frame

def process_frame(frame, threshold_value=50, display_height = 360, display_width = 540):
    """
    Threshold the captured frame to ignore pixels below a certain luminance value.

    :param frame: The captured frame (numpy array).
    :param threshold_value: The luminance threshold to ignore pixels (default is 50).
    :return: Processed frame with thresholding applied.
    """
    # Convert the frame to grayscale
    gray_frame = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)

    # Apply the thresholding operation
    _, thresholded_frame = cv2.threshold(gray_frame, threshold_value, 255, cv2.THRESH_BINARY)

    # Invert the thresholded frame to detect bright areas (white areas in the original)
    thresholded_frame = cv2.bitwise_not(thresholded_frame)
    
    # Set up the SimpleBlobDetector with default parameters
    detector_params = cv2.SimpleBlobDetector_Params()
    detector_params.filterByArea = True
    detector_params.minArea = 300  # Minimum blob area (can be adjusted)
    detector_params.filterByCircularity = False
    detector_params.filterByConvexity = False
    detector_params.filterByInertia = False

    # Create the detector object
    detector = cv2.SimpleBlobDetector_create(detector_params)

    # Detect blobs
    keypoints = detector.detect(thresholded_frame)

    # Draw the blobs on the frame (optional: you can display only the blobs)
    frame_with_blobs = cv2.drawKeypoints(thresholded_frame, keypoints, np.array([]), (0, 0, 255), cv2.DRAW_MATCHES_FLAGS_DRAW_RICH_KEYPOINTS)
    # Resize the frame for display (downscale to reduce display resolution)
    display_frame = cv2.resize(frame, (display_width, display_height))
    return display_frame

def find_center_of_mass(frame, threshold_value=50, display_height = 360, display_width = 540):
    """
    Find the center of mass of the image, using thresholding to create a binary mask.

    :param frame: The captured frame (numpy array).
    :param threshold_value: The luminance threshold to create the binary mask (default is 50).
    :return: The coordinates of the center of mass (cx, cy), or None if no valid centroid is found.
    """
    # Convert the frame to grayscale
    gray_frame = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)

    # Apply the thresholding operation
    _, thresholded_frame = cv2.threshold(gray_frame, threshold_value, 255, cv2.THRESH_BINARY)

    # Calculate moments of the thresholded image
    moments = cv2.moments(thresholded_frame)

    # Calculate the centroid (cx, cy) from the moments
    if moments['m00'] != 0:  # Avoid division by zero
        cx = int(moments['m10'] / moments['m00'])
        cy = int(moments['m01'] / moments['m00'])
        return cx, cy
    else:
        return None  # If no mass is detected, return None
    
def process_for_display_frame(raw_frame, new_width=480, new_height=640):
    """
    Process the raw captured frame:
    1. Rotate the frame 90 degrees clockwise.
    2. Resize the frame to the specified width and height.

    :param raw_frame: The raw input frame from a video capture.
    :param new_width: The desired width after resizing.
    :param new_height: The desired height after resizing.
    :return: Processed frame ready for display.
    """
    # Rotate the frame 90 degrees clockwise
    rotated_frame = cv2.rotate(raw_frame, cv2.ROTATE_90_COUNTERCLOCKWISE)
    
    # Resize the rotated frame
    processed_frame = cv2.resize(rotated_frame, (new_width, new_height))
    
    return processed_frame





#
#
#
class CameraCapture:
    def __init__(self, camera_id, coordinates=(0, 0, 0)):
        """
        Initialize the camera capture object with its ID and coordinates.

        :param camera_id: The ID of the camera (usually an integer or filename).
        :param coordinates: The (x, y, z) coordinates for the camera's position.
        """
        self.camera_id = camera_id
        self.coordinates = coordinates
        self.capture = cv2.VideoCapture(camera_id)  # Open the camera using its ID

        if not self.capture.isOpened():
            print(f"Error: Could not open camera {camera_id}")
            self.capture = None
        
        # Initialize frames to None
        self.raw_frame = None
        self.processed_frame = None
        self.display_frame = None

    def capture_frame(self):
        """
        Capture a new frame from the camera, storing it as the raw frame.
        """
        if self.capture is not None:
            ret, frame = self.capture.read()
            if ret:
                self.raw_frame = frame
                self.processed_frame = self.process_frame(self.raw_frame)
                self.display_frame = self.processed_frame
            else:
                print(f"Error: Failed to capture frame from camera {self.camera_id}")

    def process_frame(self, raw_frame, new_width=640, new_height=480):
        """
        Process the raw frame (rotate and resize).

        :param raw_frame: The raw input frame.
        :param new_width: The width of the frame after resizing.
        :param new_height: The height of the frame after resizing.
        :return: The processed frame.
        """
        # Rotate the frame 90 degrees clockwise
        rotated_frame = cv2.rotate(raw_frame, cv2.ROTATE_90_CLOCKWISE)
        
        # Resize the rotated frame
        processed_frame = cv2.resize(rotated_frame, (new_width, new_height))
        
        return processed_frame

    def release(self):
        """
        Release the camera capture when done.
        """
        if self.capture is not None:
            self.capture.release()


#
#
#
class CameraPylon:
    def __init__(self, camera_id_top, camera_id_bottom, z_offset, coordinates_top=(0, 0, 0), coordinates_bottom=(0, 0, 0)):
        """
        Initialize the camera pylon consisting of two cameras.

        :param camera_id_top: Camera ID for the top camera.
        :param camera_id_bottom: Camera ID for the bottom camera.
        :param z_offset: The vertical distance between the top and bottom cameras.
        :param coordinates_top: The (x, y, z) coordinates for the top camera.
        :param coordinates_bottom: The (x, y, z) coordinates for the bottom camera.
        """
        self.top_camera = CameraCapture(camera_id_top, coordinates=coordinates_top)
        self.bottom_camera = CameraCapture(camera_id_bottom, coordinates=coordinates_bottom)
        self.z_offset = z_offset

    def capture_frames(self):
        """
        Capture frames from both the top and bottom cameras.
        """
        self.top_camera.capture_frame()
        self.bottom_camera.capture_frame()

    def get_processed_frames(self):
        """
        Get the processed frames from both cameras.
        :return: Tuple (processed_top_frame, processed_bottom_frame)
        """
        return self.top_camera.processed_frame, self.bottom_camera.processed_frame

    def combine_frames_side_by_side(self):
        """
        Combine the processed frames of both cameras side by side.

        :return: A single combined frame (side by side).
        """
        top_frame = self.top_camera.processed_frame
        bottom_frame = self.bottom_camera.processed_frame

        if top_frame is not None and bottom_frame is not None:
            # Concatenate both frames horizontally (side by side)
            combined_frame = np.hstack((top_frame, bottom_frame))
            return combined_frame
        else:
            return None

    def release(self):
        """
        Release the resources of both cameras.
        """
        self.top_camera.release()
        self.bottom_camera.release()

