import os
import subprocess
import warnings

# Set environment variable to suppress warnings globally
os.environ["PYTHONWARNINGS"] = "ignore:.*iCCP.*:UserWarning"

import cv2
import numpy as np
import time


def disable_exposure_dynamic_framerate(camera_id):
    """
    Disables the UVC exposure_dynamic_framerate control via v4l2-ctl -
    OpenCV has no CAP_PROP for it, since it's outside the standard
    property set it exposes. Confirmed by testing (v4l2-ctl
    --list-ctrls) that this control defaults OFF on the hardware but was
    found ON, and while it's on, the camera's firmware silently
    overrides CAP_PROP_EXPOSURE with its own dynamically-chosen value
    regardless of what's requested (and regardless of
    CAP_PROP_AUTO_EXPOSURE=1 "Manual Mode", which is a separate control)
    - every exposure request after the first one in a process converged
    to the exact same value no matter what was asked for. Silently
    no-ops if v4l2-ctl isn't available or the device doesn't expose this
    control (e.g. a non-UVC or different camera model).
    """
    try:
        subprocess.run(
            ['v4l2-ctl', '-d', f'/dev/video{camera_id}', '-c', 'exposure_dynamic_framerate=0'],
            capture_output=True, timeout=2)
    except (FileNotFoundError, subprocess.TimeoutExpired):
        pass


def initialize_video_capture(camera_ids):
    """
    Initialize the video capture objects for multiple cameras and check if each is available.

    :param camera_ids: List of camera IDs (e.g., [0, 1] for two cameras).
    :return: List of video capture objects if successful, None if any camera fails to initialize.
    """
    captures = []
    for camera_id in camera_ids:
        disable_exposure_dynamic_framerate(camera_id)
        cap = cv2.VideoCapture(camera_id, cv2.CAP_V4L2)  # Use V4L2 backend

        if not cap.isOpened():
            print(f"Error: Could not open camera {camera_id}.")
            # Release whatever already opened successfully - otherwise
            # those devices leak held-open (confirmed by testing: a prior
            # failed multi-camera open left earlier cameras in the list
            # locked until the whole process was killed, blocking every
            # later attempt to use them).
            for opened_cap in captures:
                opened_cap.release()
            return None
        else:
            print(f"Success: Opened camera {camera_id}.")

        captures.append(cap)
    return captures

def set_camera_settings(captures, width=1920, height=1080, exposure=666, gain=255, focus=30, fps=None):
    """
    Set various camera settings for all cameras in the list.

    fps matters more than it looks: confirmed by testing that a
    requested exposure silently gets clamped down by the camera's own
    firmware to whatever fits within 1/fps (a frame can never take
    longer to expose than its own period), *regardless* of
    CAP_PROP_AUTO_EXPOSURE's manual-mode setting - the readback even lags
    a few frames before visibly drifting down, so a single immediate
    get() after set() can appear to confirm a value that isn't really
    sustained. At the driver's default ~30fps, exposure=666 (66.6ms) was
    being silently clamped to ~312 (31.2ms) within the first few frames,
    which had been true for the entire project without anyone noticing -
    every dwell computed from the assumption that 666 was real was
    therefore an underestimate of the true frame period. Pass a lower
    fps (e.g. 10) to actually unlock a longer real exposure.

    Explicitly disables auto-exposure and autofocus before setting manual
    values - confirmed via v4l2-ctl that these C920x cameras default to
    auto_exposure=3 (Aperture Priority/auto) and focus_automatic_continuous=1,
    under which exposure_time_absolute and focus_absolute both show
    flags=inactive and silently ignore whatever CAP_PROP_EXPOSURE/
    CAP_PROP_FOCUS are set to. Every capture done before this fix
    (including the mapping sweeps) had exposure, gain, and focus all free
    to drift/hunt frame-to-frame - exposure/gain based on overall scene
    brightness (which changes constantly as different LEDs light up in
    different positions during a sweep), focus by continuously re-hunting
    for sharpness. Both are real sources of centroid noise/bias
    independent of any physical calibration issue - continuous autofocus
    in particular can shift a lens's effective optical axis slightly while
    hunting, which would show up as position error, not just blur.

    Defaults (666, 255, 30) match what auto-exposure/autofocus had been
    converging to in practice for the real tree scene - confirmed via
    testing to reliably detect real LEDs across multiple string positions
    on both cameras. A short/low-gain exposure (150, 0) was tried first
    and failed to detect anything at all; gain=255 is maxed and likely
    adds real sensor noise on top of being stable now, so there's still
    headroom to tune exposure/gain down with more careful testing later,
    but locking these to known-working fixed values (vs. left free to
    drift) is the actual fix that matters here.

    :param captures: List of video capture objects.
    :param width: Desired frame width (default is 1920).
    :param height: Desired frame height (default is 1080).
    :param exposure: Manual exposure_time_absolute, V4L2 100us units.
    :param gain: Manual gain, 0-255.
    :param focus: Manual focus_absolute, V4L2 units (0=infinity, higher=closer).
    :return: None
    """
    for cap in captures:
        # Request MJPEG (compressed) instead of the default raw format.
        # With 4 UVC webcams sharing a USB bus, raw YUYV at any reasonable
        # resolution blows the isochronous bandwidth budget (seen as kernel
        # "Not enough bandwidth for altsetting" errors and cameras dropping
        # off the bus). MJPEG needs a fraction of the bandwidth. Must be set
        # before width/height so the driver negotiates the resolution against
        # the right format.
        cap.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*'MJPG'))
        cap.set(cv2.CAP_PROP_FRAME_WIDTH, width)
        cap.set(cv2.CAP_PROP_FRAME_HEIGHT, height)
        if fps is not None:
            cap.set(cv2.CAP_PROP_FPS, fps)  # set before exposure - caps the exposure ceiling
        cap.set(cv2.CAP_PROP_AUTO_EXPOSURE, 1)  # V4L2 UVC: 1 = Manual Mode (3 = auto, the default, was silently winning)
        cap.set(cv2.CAP_PROP_EXPOSURE, exposure)
        cap.set(cv2.CAP_PROP_GAIN, gain)
        cap.set(cv2.CAP_PROP_AUTOFOCUS, 0)
        cap.set(cv2.CAP_PROP_FOCUS, focus)

    # Print the current settings to verify (optional)
    for idx, cap in enumerate(captures):
        actual_width = cap.get(cv2.CAP_PROP_FRAME_WIDTH)
        actual_height = cap.get(cv2.CAP_PROP_FRAME_HEIGHT)
        actual_auto_exposure = cap.get(cv2.CAP_PROP_AUTO_EXPOSURE)
        actual_exposure = cap.get(cv2.CAP_PROP_EXPOSURE)
        actual_gain = cap.get(cv2.CAP_PROP_GAIN)
        actual_autofocus = cap.get(cv2.CAP_PROP_AUTOFOCUS)
        actual_focus = cap.get(cv2.CAP_PROP_FOCUS)
        actual_fourcc = int(cap.get(cv2.CAP_PROP_FOURCC))
        actual_fourcc_str = "".join(chr((actual_fourcc >> (8 * i)) & 0xFF) for i in range(4))
        actual_fps = cap.get(cv2.CAP_PROP_FPS)
        print(f"Camera {idx}: Width={actual_width}, Height={actual_height}, FPS={actual_fps}, "
              f"AutoExposure={actual_auto_exposure}, Exposure={actual_exposure}, "
              f"Gain={actual_gain}, AutoFocus={actual_autofocus}, Focus={actual_focus}, "
              f"FOURCC={actual_fourcc_str}")

def drain_for(caps, duration_s):
    """
    Actively empties each camera's internal V4L2 buffer for duration_s by
    continuously discarding frames in a round-robin across all cams,
    instead of blindly time.sleep()ing while the camera keeps streaming
    unread frames into a growing backlog.

    Confirmed via mapping/investigate_frame_timing.py: cap.read() returns
    near-instantly (~4-8ms) while popping already-buffered (stale, from
    before whatever state change just happened) frames, then jumps to a
    real ~27-31ms block once it's actually waiting on a fresh one - the
    old pattern (sleep, then a single flush read, then the "real" read)
    was landing on a still-stale frame at random depending on how deep
    the backlog happened to be, which is a strong candidate for much of
    the frame-to-frame detection flakiness seen in testing (the same LED,
    same physical setup, flipping between ZERO/OK/AMBIG across identical
    repeated captures). This serves double duty: satisfies the physical
    settling-time requirement AND guarantees the buffer is empty by the
    time duration_s elapses, so the very next real read is fresh.

    :param caps: list of cv2.VideoCapture objects to round-robin drain.
    :param duration_s: total wall-clock time to spend draining.
    """
    t_end = time.time() + duration_s
    while time.time() < t_end:
        for cap in caps:
            cap.read()


def drain_until_live(caps, max_duration_s=1.0, live_threshold_s=0.012, consecutive_needed=2):
    """
    Drains each camera until its OWN reads directly show the live-frame
    latency signature, instead of assuming a precomputed dwell time was
    long enough. A backlogged (stale, pre-transition) read returns
    near-instantly (~4-8ms observed); once the backlog is drained, reads
    start genuinely blocking for close to the camera's real frame period
    (~27-31ms+ at fps=30) - see investigate_frame_timing.py. A camera is
    marked live once `consecutive_needed` reads in a row take at least
    live_threshold_s (comfortably above backlog-draining latency,
    comfortably below a real live frame time even at fps=30) - a single
    slow read could be a scheduling hiccup, not real freshness, so this
    asks for two in a row before trusting it.

    This exists because computing dwell from a fixed formula (exposure,
    fps, camera count) was confirmed by testing to sometimes make
    results dramatically WORSE, not better, in ways that weren't fully
    explained (e.g., scaling dwell up for a 4-camera round-robin, a
    logically-justified change, tanked solve rates from ~100/1000 to
    ~1-10/1000). Measuring the actual state directly, instead of
    predicting it from a model that's proven unreliable, sidesteps
    needing to get that model right.

    Cameras that never reach the live signature within max_duration_s
    (e.g., genuinely disconnected or badly misbehaving) are given up on
    at the deadline rather than blocking the whole sweep forever - the
    caller's subsequent real read may then land on a non-live frame for
    that camera, same as it would have under the old dwell-based
    approach, so this never behaves worse than that baseline.

    :param caps: list of cv2.VideoCapture objects to drain independently.
    :param max_duration_s: safety cap on total time spent per call.
    :param live_threshold_s: read latency above which a read counts as "live".
    :param consecutive_needed: consecutive live-latency reads required before stopping a camera.
    :return: dict {id(cap): bool} - which cameras reached the live state before the deadline.
    """
    deadline = time.time() + max_duration_s
    streaks = {id(cap): 0 for cap in caps}
    live = {id(cap): False for cap in caps}
    while time.time() < deadline and not all(live.values()):
        for cap in caps:
            if live[id(cap)]:
                continue
            t0 = time.time()
            cap.read()
            elapsed = time.time() - t0
            if elapsed >= live_threshold_s:
                streaks[id(cap)] += 1
                if streaks[id(cap)] >= consecutive_needed:
                    live[id(cap)] = True
            else:
                streaks[id(cap)] = 0
    return live


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
    
def find_single_blob_centroid(frame, background=None, threshold_value=250, min_area=20, close_kernel_size=3):
    """
    Centroid detection for the mapping sweep, stricter than
    find_center_of_mass(): requires exactly one bright blob above
    min_area after optional background subtraction, since a real sweep
    can pick up static room-light/ornament-reflection artifacts that a
    plain single-frame threshold can't tell apart from the lit LED.

    :param frame: captured frame (numpy array, BGR).
    :param background: an all-LEDs-off reference frame from the same
        camera (captured once per sweep), or None to skip subtraction.
    :param threshold_value: luminance threshold after subtraction.
    :param min_area: minimum blob pixel area to count as a real detection.
    :param close_kernel_size: morphological CLOSE kernel size applied to
        the mask before counting components. A single genuinely-lit LED's
        blob was confirmed (via diagnose_blobs.py, visual inspection of
        the actual mask) to sometimes come apart into 2+ disconnected
        pixel islands a few px apart under MJPG compression noise -
        connectedComponentsWithStats then counts it as multiple blobs and
        the caller rejects it as ambiguous, even though only one real
        light source is present. Closing bridges gaps that small without
        merging genuinely separate blobs (seen in the same diagnostic
        images sitting tens of px apart). Pass 0 to disable.
    :return: (cx, cy, blob_count, largest_area). cx/cy are None unless
        blob_count == 1 - blob_count/largest_area are still reported when
        ambiguous (0 or >1 blobs) so the caller can log why it was rejected.
    """
    gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
    if background is not None:
        bg_gray = cv2.cvtColor(background, cv2.COLOR_BGR2GRAY) if background.ndim == 3 else background
        gray = cv2.subtract(gray, bg_gray)

    _, mask = cv2.threshold(gray, threshold_value, 255, cv2.THRESH_BINARY)
    if close_kernel_size > 0:
        kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (close_kernel_size, close_kernel_size))
        mask = cv2.morphologyEx(mask, cv2.MORPH_CLOSE, kernel)
    num_labels, labels, stats, centroids = cv2.connectedComponentsWithStats(mask, connectivity=8)

    # label 0 is the background component; keep only real blobs >= min_area
    blob_areas = [stats[i, cv2.CC_STAT_AREA] for i in range(1, num_labels)
                  if stats[i, cv2.CC_STAT_AREA] >= min_area]
    blob_count = len(blob_areas)
    # cv2 stats are numpy int32 - cast to a native float so callers (e.g.
    # sqlite3) don't silently serialize a numpy scalar as raw bytes via the
    # buffer protocol instead of a real number (confirmed: this happened).
    largest_area = float(max(blob_areas)) if blob_areas else 0.0

    if blob_count != 1:
        return None, None, blob_count, largest_area

    idx = 1 + [stats[i, cv2.CC_STAT_AREA] for i in range(1, num_labels)].index(largest_area)
    cx, cy = centroids[idx]
    return float(cx), float(cy), blob_count, float(largest_area)


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

