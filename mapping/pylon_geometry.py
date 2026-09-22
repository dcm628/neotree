"""
Camera/pylon geometry model and multi-view triangulation, used to turn 2D
centroid pixel coordinates from a pylon's cameras into a 3D LED position
estimate (in that pylon's own local coordinate frame - cross-pylon/
cross-sweep alignment into one shared frame is a separate, later step).

No formal stereo calibration is being done (deliberate, for now - see
project discussion): intrinsics are derived from the camera's spec'd
diagonal FOV rather than measured, and each pylon's two cameras are assumed
perfectly parallel (no relative tilt/pan/roll) and separated by exactly
PYLON_CAMERA_SPACING_MM along the pylon's own vertical rail. This bakes in
real systematic error, not just noise - every solved point/session is
tagged with a calibration_source so this can be revisited without silently
mixing calibrated and uncalibrated data later.

Units: millimeters, matching the firmware's cylindrical position config
(see neotree_serial.write_tree_pos_cylindrical).

Pylon-local frame (right-handed): origin at the BOTTOM camera's optical
center.
    X = camera "right" (raw image +v direction - the cameras are mounted
        rotated 90 degrees/portrait, confirmed physically and by real
        captured data, see CameraModel.ray_direction)
    Y = camera "forward" (viewing direction, into the tree)
    Z = "up" (toward the top camera, raw image +u direction)
The top camera sits at (0, 0, PYLON_CAMERA_SPACING_MM) in this frame, with
the same orientation as the bottom camera (parallel-cameras assumption).
"""
import math
from dataclasses import dataclass

import numpy as np

# Logitech C920x HD Pro spec'd diagonal FOV. Placeholder in the same sense
# as the 24" pylon spacing - good enough to get real units into the
# triangulation now, worth replacing with an actual calibration later.
C920X_DIAGONAL_FOV_DEG = 78.0

PYLON_CAMERA_SPACING_MM = 592.0  # measured (was a 24"/609.6mm placeholder), per-pylon override supported


def focal_length_px(width_px, height_px, diagonal_fov_deg=C920X_DIAGONAL_FOV_DEG):
    """
    Pinhole focal length in pixels, derived from the sensor's spec'd
    diagonal FOV and the actual capture resolution (must match whatever
    resolution the frames being measured were captured at).
    """
    diag_px = math.hypot(width_px, height_px)
    return (diag_px / 2.0) / math.tan(math.radians(diagonal_fov_deg) / 2.0)


@dataclass
class CameraModel:
    """A single camera's intrinsics + its pose within a pylon's local frame."""
    width_px: int
    height_px: int
    focal_px: float
    origin: np.ndarray       # (3,) position in pylon-local frame
    principal_x: float = None
    principal_y: float = None

    def __post_init__(self):
        if self.principal_x is None:
            self.principal_x = self.width_px / 2.0
        if self.principal_y is None:
            self.principal_y = self.height_px / 2.0
        self.origin = np.asarray(self.origin, dtype=float)

    def ray_direction(self, u, v):
        """
        Unit direction (in pylon-local frame) of the ray through raw pixel
        (u, v).

        The physical cameras are mounted rotated 90 degrees (portrait, to
        cover more of the tree's height per frame) - confirmed by the user
        and by real captured data: for the same LED, the top and bottom
        camera's raw *u* coordinate differs by a large, consistent amount
        matching the ~592mm vertical baseline (mean ~168px, stdev ~33px
        across 111 real LEDs), while raw *v* barely differs (mean ~35px,
        stdev ~22px - consistent with noise, not a baseline). So raw u is
        real-world vertical, not raw v as a normal (unrotated) camera would
        have it. Re-triangulating the same real data with u/v swapped
        dropped the mean ray residual from 283.7mm to 66.1mm (out of a
        ~600mm z-span) - confirms the swap; see git history for the
        before/after if this ever needs re-deriving.
        """
        right = (v - self.principal_y) / self.focal_px
        up = (u - self.principal_x) / self.focal_px
        d = np.array([right, 1.0, up], dtype=float)
        return d / np.linalg.norm(d)

    def project(self, point_pylon_frame):
        """Inverse of ray_direction: 3D pylon-local point -> (u, v) pixel. Used only by the self-test."""
        p = np.asarray(point_pylon_frame, dtype=float) - self.origin
        if p[1] <= 0:
            return None  # behind the camera
        right = p[0] / p[1]
        up = p[2] / p[1]
        v = right * self.focal_px + self.principal_y
        u = up * self.focal_px + self.principal_x
        return u, v


def make_pylon_cameras(width_px, height_px, spacing_mm=PYLON_CAMERA_SPACING_MM,
                        diagonal_fov_deg=C920X_DIAGONAL_FOV_DEG):
    """Build the (bottom, top) CameraModel pair for one pylon."""
    f = focal_length_px(width_px, height_px, diagonal_fov_deg)
    bottom = CameraModel(width_px, height_px, f, origin=(0.0, 0.0, 0.0))
    top = CameraModel(width_px, height_px, f, origin=(0.0, 0.0, spacing_mm))
    return bottom, top


@dataclass
class TriangulationResult:
    point: np.ndarray          # (3,) solved position, pylon-local frame, mm
    covariance: np.ndarray     # (3,3) approximate covariance, mm^2
    ray_residual_mm: float     # RMS perpendicular distance from the solved point to each ray
    n_rays: int


def _lstsq_point(origins, directions):
    """
    Least-squares closest point to a set of 3D rays (each origin o_i, unit
    direction d_i). For each ray, the point P minimizing perpendicular
    distance satisfies (I - d d^T)(P - o) = 0; summing these normal
    equations across rays gives a standard linear least-squares system
    A P = b. Point-only - no uncertainty here, see _triangulate() below for
    why covariance needs more than this.
    """
    A = np.zeros((3, 3))
    b = np.zeros(3)
    for o, d in zip(origins, directions):
        d = d / np.linalg.norm(d)
        M = np.eye(3) - np.outer(d, d)
        A += M
        b += M @ o
    point, *_ = np.linalg.lstsq(A, b, rcond=None)
    return point


def _rays_from_pixels(cameras, pixels):
    origins = [c.origin for c in cameras]
    directions = [c.ray_direction(u, v) for c, (u, v) in zip(cameras, pixels)]
    return origins, directions


def _triangulate(cameras, pixels, pixel_sigma_px):
    origins, directions = _rays_from_pixels(cameras, pixels)
    point = _lstsq_point(origins, directions)

    residuals = []
    for o, d in zip(origins, directions):
        d = d / np.linalg.norm(d)
        v = point - o
        perp = v - np.dot(v, d) * d
        residuals.append(np.linalg.norm(perp))
    ray_residual_mm = float(np.sqrt(np.mean(np.square(residuals))))

    # Covariance via a numerical Jacobian of the triangulated point w.r.t.
    # each pixel coordinate (central differences), rather than an
    # analytical shortcut. This matters: an earlier version approximated
    # covariance from the ray-geometry matrix alone, scaled by pixel noise
    # - which is dimensionally wrong (it never scales with distance to the
    # point) and gave physically implausible sub-micron uncertainties at
    # real-world (multi-meter) ranges. The correct behavior - e.g. depth
    # uncertainty growing with range^2 / baseline, a standard stereo-vision
    # result - falls straight out of actually perturbing pixel coordinates
    # and re-solving, since the triangulation is nonlinear in pixel space.
    eps_px = 0.5
    n_params = 2 * len(cameras)
    J = np.zeros((3, n_params))
    for k in range(n_params):
        cam_idx, axis = divmod(k, 2)  # axis 0=u, 1=v
        px_plus = [list(p) for p in pixels]
        px_minus = [list(p) for p in pixels]
        px_plus[cam_idx][axis] += eps_px
        px_minus[cam_idx][axis] -= eps_px
        pt_plus = _lstsq_point(*_rays_from_pixels(cameras, px_plus))
        pt_minus = _lstsq_point(*_rays_from_pixels(cameras, px_minus))
        J[:, k] = (pt_plus - pt_minus) / (2 * eps_px)

    covariance = (pixel_sigma_px ** 2) * (J @ J.T)

    return TriangulationResult(point=point, covariance=covariance,
                                ray_residual_mm=ray_residual_mm, n_rays=len(cameras))


def triangulate_pylon_observation(bottom_cam, top_cam, bottom_px, top_px, pixel_sigma_px=1.0):
    """
    Convenience wrapper for the common 2-camera (single pylon) case.

    :param bottom_cam, top_cam: CameraModel.
    :param bottom_px, top_px: (u, v) pixel centroid in each camera, or None
        if that camera didn't find a valid centroid for this LED.
    :return: TriangulationResult, or None if fewer than 2 of the 2 cameras
        found a centroid (a single ray alone can't be triangulated).
    """
    cameras, pixels = [], []
    if bottom_px is not None:
        cameras.append(bottom_cam)
        pixels.append(bottom_px)
    if top_px is not None:
        cameras.append(top_cam)
        pixels.append(top_px)
    if len(cameras) < 2:
        return None
    return _triangulate(cameras, pixels, pixel_sigma_px)


def pylon_to_coarse_tree_frame(point_pylon, standoff_mm, ground_offset_mm):
    """
    Deliberately crude placeholder for turning one pylon's local-frame
    solves into the tree's own frame (trunk = vertical Z axis, radius/omega
    measured from it) - a manual stand-in until real cross-session
    registration (Kabsch/RANSAC onto a shared frame, then a fit onto the
    tree's actual trunk axis) exists.

    Assumes the pylon is boresighted straight at the trunk - i.e. the trunk
    is a vertical line at pylon-local (x=0, y=standoff_mm) for every
    height - so only a horizontal shift (by the standoff distance from the
    bottom camera to the trunk) and a vertical shift (by how far the
    bottom camera sits above the tree's base) are applied. No rotation:
    with a single pylon, radius = sqrt(x^2 + y'^2) and omega = atan2(y', x)
    are already self-consistent around *a* vertical axis without one -
    rotation only starts to matter once a second pylon/sweep needs its
    omega to agree with this one, which is real alignment work, not this.

    :param point_pylon: (3,) array-like, pylon-local (x, y, z) mm.
    :param standoff_mm: horizontal distance from the bottom camera to the
        trunk, measured along the camera's forward (Y) axis.
    :param ground_offset_mm: height of the bottom camera above the tree's
        base (added so z=0 lands at the tree base, not the camera).
    :return: (3,) array, coarse tree-frame (x, y, z) mm.
    """
    x, y, z = point_pylon
    return np.array([x, y - standoff_mm, z + ground_offset_mm])


def _self_test():
    """Synthetic round-trip check: project a known point, triangulate it back."""
    width, height = 1208, 680
    bottom, top = make_pylon_cameras(width, height)

    test_points = [
        (150.0, 2000.0, 300.0),
        (-400.0, 3500.0, 900.0),
        (0.0, 1200.0, 12.0),
    ]
    print(f"focal_px = {bottom.focal_px:.1f}")
    max_err = 0.0
    for pt in test_points:
        pt = np.array(pt)
        u_b, v_b = bottom.project(pt)
        u_t, v_t = top.project(pt)
        result = triangulate_pylon_observation(bottom, top, (u_b, v_b), (u_t, v_t))
        err = np.linalg.norm(result.point - pt)
        max_err = max(max_err, err)
        print(f"  true={pt} solved={result.point.round(3)} "
              f"err_mm={err:.6f} ray_residual_mm={result.ray_residual_mm:.6f} "
              f"cov_diag={result.covariance.diagonal().round(6)}")
        assert err < 1e-6, f"triangulation round-trip failed: err={err}"
        assert result.ray_residual_mm < 1e-6

    # Sanity check on the anisotropy claim: with a vertical-only baseline,
    # depth (Y, along the shared viewing direction) should be far less
    # constrained than the lateral axes for a point straight ahead, and its
    # magnitude should roughly match the standard stereo-vision result
    # sigma_Z ~= Z^2 * sigma_pixel / (f * baseline) - not just "large", but
    # the right order of magnitude for a real depth range.
    Z_range = 3000.0
    pt = np.array([0.0, Z_range, PYLON_CAMERA_SPACING_MM / 2])
    u_b, v_b = bottom.project(pt)
    u_t, v_t = top.project(pt)
    pixel_sigma = 1.0
    result = triangulate_pylon_observation(bottom, top, (u_b, v_b), (u_t, v_t), pixel_sigma_px=pixel_sigma)
    cov_x, cov_y, cov_z = result.covariance.diagonal()
    sigma_y = math.sqrt(cov_y)
    expected_sigma_y = Z_range ** 2 * pixel_sigma / (bottom.focal_px * PYLON_CAMERA_SPACING_MM)
    print(f"\nanisotropy check at {pt}: cov_x={cov_x:.2f} cov_y(depth)={cov_y:.2f} cov_z={cov_z:.2f}")
    print(f"  sigma_y={sigma_y:.1f}mm vs standard-formula estimate={expected_sigma_y:.1f}mm")
    assert cov_y > cov_x and cov_y > cov_z, "expected depth (Y) to be the least-constrained axis"
    ratio = sigma_y / expected_sigma_y
    assert 0.5 < ratio < 2.0, f"depth uncertainty off from the standard stereo estimate by {ratio:.2f}x"

    print(f"\nAll self-tests passed. max round-trip error = {max_err:.2e} mm")


if __name__ == "__main__":
    _self_test()
