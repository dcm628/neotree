"""
Shared ChArUco board definition, used by every calibration script so
there's exactly one place that has to match the physical board.

Matches the board actually ordered (FoamCorePrint, generated preview
verified 2026-09-23): 15 columns x 10 rows of squares, 20mm checker
width, 15mm marker width, dictionary confirmed by decoding the real
generated board image against candidate OpenCV dictionaries - only
DICT_4X4_100/_250/_1000 could decode all 75 markers (IDs 0-74) present;
DICT_4X4_50 topped out at 50. DICT_4X4_100 is the smallest of those that
works and is guaranteed to match whichever of the three FoamCorePrint's
generator actually used internally, since OpenCV's predefined ArUco
dictionaries are nested (each larger one's first N patterns are
byte-identical to the smaller one of size N).

If the board is ever regenerated with different settings, update these
constants (and nothing else needs to change - every script imports from
here).
"""
import cv2

SQUARES_X = 15  # columns (board width direction)
SQUARES_Y = 10  # rows (board height direction)
CHECKER_MM = 20.0
MARKER_MM = 15.0
ARUCO_DICT = cv2.aruco.DICT_4X4_100

# Minimum charuco corners detected for a pose to count as usable -
# roughly a quarter of the (15-1)*(10-1)=126 possible interior corners.
# Low enough to accept genuinely partial/oblique views (the whole point
# of using ChArUco over a plain checkerboard), high enough that a
# severely clipped or edge-on view doesn't get accepted as a real pose.
MIN_CORNERS_FOR_CAPTURE = 30


def make_board():
    dictionary = cv2.aruco.getPredefinedDictionary(ARUCO_DICT)
    return cv2.aruco.CharucoBoard(
        (SQUARES_X, SQUARES_Y), CHECKER_MM / 1000.0, MARKER_MM / 1000.0, dictionary)


def make_detector():
    board = make_board()
    params = cv2.aruco.DetectorParameters()
    # Wider adaptive-threshold window range than the OpenCV default -
    # the board will be seen across a range of real-world distances
    # (near for good corner density, far to represent actual pylon
    # working distance), so marker size in pixels varies a lot between
    # captures.
    params.adaptiveThreshWinSizeMin = 3
    params.adaptiveThreshWinSizeMax = 53
    params.adaptiveThreshWinSizeStep = 4
    charuco_params = cv2.aruco.CharucoParameters()
    return cv2.aruco.CharucoDetector(board, charuco_params, params)
