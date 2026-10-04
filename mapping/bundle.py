"""
Bundle adjustment for LED mapping: every station's pose and every LED's
position solved together from all the pixel observations (docs/
CALIBRATION.md, "Phased mapping").

A *rig* is one pylon: two calibrated cameras (bottom and top) in a fixed
relative pose (calibrate_stereo.py). A *station* is a rig placed somewhere
for one sweep. Moving a rig between stations keeps its stereo geometry, so
a rig's two cameras move together; its scale (the baseline) comes from the
board calibration and fixes the whole map's scale.

Why this beats a single pylon's stereo: one pylon at one depth can't pin
down the rotation that shifts every disparity equally (it costs ~Z^2/b of
depth per radian). An LED seen from stations 60 degrees apart is fixed by
the other station's sideways view instead - and that, run jointly, also
corrects each rig's small stereo angle error (refine_rigs).

Frames: OpenCV camera frames (x right, y down, z forward in the raw image),
millimetres. A station's pose maps world -> its bottom camera:
X_bottom = R_s @ X + t_s; its top camera: X_top = R_rel @ X_bottom + T_rel.

Intrinsics (refine_intrinsics): each camera's focal lengths, optical
centre and first two distortion terms can be refined too, held near the
board calibration by priors. At 2.3 m, the board's typical 0.2% focal and
1.5 px centre errors alone cost 3-4 mm; with hundreds of LEDs seen from
several stations, those few numbers per camera are well determined.

Phase 2 (anchored): anchors are LEDs with known positions and uncertainty
(from phase 1); they become soft priors, define the frame (no station is
held fixed), and each station starts from a perspective-n-point solve of
its cameras on the anchors it sees. Wiring limits (neighbouring LEDs at
most L mm apart) can be added as one-sided soft constraints.
"""
from dataclasses import dataclass, field

import cv2
import numpy as np
from scipy.optimize import least_squares
from scipy.sparse import lil_matrix


@dataclass
class Rig:
    name: str
    K_bottom: np.ndarray
    D_bottom: np.ndarray
    K_top: np.ndarray
    D_top: np.ndarray
    R_rel: np.ndarray          # top from bottom (OpenCV frames)
    T_rel: np.ndarray          # mm


@dataclass
class Observations:
    """One row per (station, camera, LED) sighting: raw pixel coordinates."""
    station: np.ndarray        # int
    cam: np.ndarray            # 0 bottom, 1 top
    led: np.ndarray            # int, LED index
    uv: np.ndarray             # (n, 2) pixels

    @staticmethod
    def from_rows(rows):
        rows = list(rows)
        a = np.array([(s, c, l) for s, c, l, _u, _v in rows], dtype=np.int64).reshape(-1, 3)
        uv = np.array([(u, v) for _s, _c, _l, u, v in rows], dtype=np.float64).reshape(-1, 2)
        return Observations(a[:, 0], a[:, 1], a[:, 2], uv)


@dataclass
class Result:
    points: dict                         # led -> (3,) world mm
    cov: dict                            # led -> (3, 3) mm^2
    n_obs: dict                          # led -> observations used
    n_stations: dict                     # led -> stations that saw it
    station_R: list                      # per station (3, 3)
    station_t: list                      # per station (3,)
    rig_delta: dict                      # rig name -> rotation vector applied to R_rel (rad)
    rms_px: float
    dropped: int                         # observations rejected as outliers
    residual_px: np.ndarray = field(default=None)   # per used observation
    intrinsics: dict = field(default=None)          # (rig, cam) -> fx fy cx cy k1 k2 p1 p2 k3, as refined


def _rot(r):
    return cv2.Rodrigues(np.asarray(r, dtype=np.float64))[0]


def _project(P, fx, fy, cx, cy, k1, k2, p1, p2, k3):
    """OpenCV's pinhole + distortion model, vectorized: camera-frame points -> pixels."""
    z = np.where(np.abs(P[:, 2]) < 1e-6, 1e-6, P[:, 2])
    x, y = P[:, 0] / z, P[:, 1] / z
    r2 = x * x + y * y
    radial = 1 + k1 * r2 + k2 * r2 * r2 + k3 * r2 * r2 * r2
    xd = x * radial + 2 * p1 * x * y + p2 * (r2 + 2 * x * x)
    yd = y * radial + p1 * (r2 + 2 * y * y) + 2 * p2 * x * y
    return np.stack([fx * xd + cx, fy * yd + cy], axis=1)


def _kabsch(P, Q):
    """R, t with R @ p + t ~ q (rows are points)."""
    pc, qc = P.mean(0), Q.mean(0)
    U, _s, Vt = np.linalg.svd((P - pc).T @ (Q - qc))
    D = np.diag([1.0, 1.0, np.sign(np.linalg.det(Vt.T @ U.T))])
    R = Vt.T @ D @ U.T
    return R, qc - R @ pc


def _robust_kabsch(P, Q, inlier_mm=25.0, rounds=6):
    keep = np.ones(len(P), bool)
    for _ in range(rounds):
        R, t = _kabsch(P[keep], Q[keep])
        err = np.linalg.norm(P @ R.T + t - Q, axis=1)
        new = err < max(inlier_mm, 3 * np.median(err[keep]))
        if new.sum() < 4 or (new == keep).all():
            break
        keep = new
    return R, t, keep


class Bundle:
    def __init__(self, rigs, station_rigs, obs, refine_rigs=True, rig_prior_mrad=3.0,
                 anchors=None, wiring=None, wiring_sigma_mm=5.0, refine_intrinsics=False,
                 focal_prior=0.004, centre_prior_px=3.0, k_prior=(0.02, 0.05)):
        """
        rigs: {name: Rig}; station_rigs: list of rig names, one per station;
        obs: Observations; anchors: {led: ((3,) mm, sigma_mm)} or None;
        wiring: [(led_i, led_j, max_mm)] or None.
        """
        self.rigs = rigs
        self.rig_names = sorted(rigs)
        self.station_rigs = list(station_rigs)
        self.n_st = len(self.station_rigs)
        self.refine_rigs = refine_rigs
        self.rig_prior = rig_prior_mrad / 1000.0
        self.anchors = anchors or {}
        self.wiring = wiring or []
        self.wiring_sigma = wiring_sigma_mm
        self.fixed_station = None if self.anchors else 0
        self.refine_intrinsics = refine_intrinsics
        # Intrinsic deltas (fx, fy, cx, cy, k1, k2) per (rig, camera), and their priors.
        self.int_sigma = np.array([0.0, 0.0, centre_prior_px, centre_prior_px, k_prior[0], k_prior[1]])
        self.focal_prior = focal_prior
        self.cam_keys = [(n, c) for n in self.rig_names for c in (0, 1)]
        self.cam_of_station = np.array([[self.cam_keys.index((self.station_rigs[s], c)) for c in (0, 1)]
                                        for s in range(self.n_st)])
        self.base_intr = []
        for n, c in self.cam_keys:
            K, D = ((rigs[n].K_bottom, rigs[n].D_bottom) if c == 0 else (rigs[n].K_top, rigs[n].D_top))
            D = np.zeros(5) if D is None else np.asarray(D, dtype=np.float64).ravel()
            D = np.concatenate([D, np.zeros(max(0, 5 - len(D)))])[:5]
            self.base_intr.append(np.array([K[0, 0], K[1, 1], K[0, 2], K[1, 2], D[0], D[1], D[2], D[3], D[4]]))
        self.base_intr = np.array(self.base_intr)
        self.obs_cam = self.cam_of_station[obs.station, obs.cam]

        # Observations, undistorted once into normalized coordinates.
        self.obs = obs
        xn = np.zeros_like(obs.uv)
        self.focal = np.zeros(len(obs.uv))
        for s in range(self.n_st):
            rig = rigs[self.station_rigs[s]]
            for c, (K, D) in enumerate(((rig.K_bottom, rig.D_bottom), (rig.K_top, rig.D_top))):
                m = (obs.station == s) & (obs.cam == c)
                if m.any():
                    xn[m] = cv2.undistortPoints(obs.uv[m].reshape(-1, 1, 2), K, D).reshape(-1, 2)
                    self.focal[m] = (K[0, 0] + K[1, 1]) / 2
        self.xn = xn
        self.leds = sorted(set(obs.led.tolist()) | set(self.anchors))
        self.led_index = {l: k for k, l in enumerate(self.leds)}

    # ---- parameter layout ----
    def _layout(self):
        self.pose_stations = [s for s in range(self.n_st) if s != self.fixed_station]
        self.n_pose = 6 * len(self.pose_stations)
        self.n_rig = 3 * len(self.rig_names) if self.refine_rigs else 0
        self.n_int = 6 * len(self.cam_keys) if self.refine_intrinsics else 0
        self.n_pts = 3 * len(self.leds)

    def _unpack(self, x):
        R = [np.eye(3)] * self.n_st
        t = [np.zeros(3)] * self.n_st
        for k, s in enumerate(self.pose_stations):
            R[s] = _rot(x[6 * k:6 * k + 3])
            t[s] = x[6 * k + 3:6 * k + 6]
        if self.fixed_station is not None:
            R[self.fixed_station], t[self.fixed_station] = self._fixed_R, self._fixed_t
        deltas = {}
        for k, name in enumerate(self.rig_names):
            deltas[name] = x[self.n_pose + 3 * k:self.n_pose + 3 * k + 3] if self.refine_rigs else np.zeros(3)
        X = x[self.n_pose + self.n_rig + self.n_int:].reshape(-1, 3)
        return R, t, deltas, X

    def _intrinsics(self, x):
        """Per camera key: fx fy cx cy k1 k2 p1 p2 k3, with any refinement applied."""
        intr = self.base_intr.copy()
        if self.refine_intrinsics:
            d = x[self.n_pose + self.n_rig:self.n_pose + self.n_rig + self.n_int].reshape(-1, 6)
            intr[:, 0] *= 1 + d[:, 0]
            intr[:, 1] *= 1 + d[:, 1]
            intr[:, 2:6] += d[:, 2:6]
        return intr

    def _camera_transforms(self, R, t, deltas):
        """Per (station, cam): rotation and translation world -> camera."""
        Rc = np.zeros((self.n_st * 2, 3, 3))
        tc = np.zeros((self.n_st * 2, 3))
        for s in range(self.n_st):
            rig = self.rigs[self.station_rigs[s]]
            Rrel = _rot(deltas[rig.name]) @ rig.R_rel
            Rc[2 * s], tc[2 * s] = R[s], t[s]
            Rc[2 * s + 1], tc[2 * s + 1] = Rrel @ R[s], Rrel @ t[s] + rig.T_rel
        return Rc, tc

    def _residuals(self, x, mask):
        R, t, deltas, X = self._unpack(x)
        Rc, tc = self._camera_transforms(R, t, deltas)
        o = self.obs
        ci = 2 * o.station[mask] + o.cam[mask]
        P = np.einsum("nij,nj->ni", Rc[ci], X[self.obs_pt[mask]]) + tc[ci]
        intr = self._intrinsics(x)[self.obs_cam[mask]]
        uv = _project(P, *intr.T)
        res = [(uv - o.uv[mask]).ravel()]
        if self.refine_rigs:
            res.append(np.concatenate([deltas[n] for n in self.rig_names]) / self.rig_prior)
        if self.refine_intrinsics:
            d = x[self.n_pose + self.n_rig:self.n_pose + self.n_rig + self.n_int].reshape(-1, 6)
            sig = np.tile(self.int_sigma, (len(d), 1))
            sig[:, 0] = sig[:, 1] = self.focal_prior
            res.append((d / sig).ravel())
        if len(self.anchor_items):
            a = np.array([X[self.led_index[l]] - p for l, (p, _s) in self.anchor_items]) / self.anchor_sigma[:, None]
            res.append(a.ravel())
        if len(self.w_i):
            d = np.linalg.norm(X[self.w_i] - X[self.w_j], axis=1)
            res.append(np.maximum(d - self.w_max, 0.0) / self.wiring_sigma)
        return np.concatenate(res)

    def _sparsity(self, mask):
        o = self.obs
        idx = np.nonzero(mask)[0]
        n_res = (2 * len(idx) + self.n_rig + self.n_int + 3 * len(self.anchor_items) + len(self.w_i))
        J = lil_matrix((n_res, self.n_pose + self.n_rig + self.n_int + self.n_pts), dtype=np.int8)
        pose_col = {s: 6 * k for k, s in enumerate(self.pose_stations)}
        rig_col = {n: self.n_pose + 3 * k for k, n in enumerate(self.rig_names)}
        int_col = self.n_pose + self.n_rig
        base = self.n_pose + self.n_rig + self.n_int
        for r, i in enumerate(idx):
            rows = (2 * r, 2 * r + 1)
            s = o.station[i]
            cols = []
            if s in pose_col:
                cols += range(pose_col[s], pose_col[s] + 6)
            if self.refine_rigs and o.cam[i] == 1:
                c0 = rig_col[self.station_rigs[s]]
                cols += range(c0, c0 + 3)
            if self.refine_intrinsics:
                c0 = int_col + 6 * self.obs_cam[i]
                cols += range(c0, c0 + 6)
            p0 = base + 3 * self.obs_pt[i]
            cols += range(p0, p0 + 3)
            for row in rows:
                J[row, cols] = 1
        row = 2 * len(idx)
        if self.refine_rigs:
            for k in range(self.n_rig):
                J[row + k, self.n_pose + k] = 1
            row += self.n_rig
        for k in range(self.n_int):
            J[row + k, int_col + k] = 1
        row += self.n_int
        for l, _a in self.anchor_items:
            p0 = base + 3 * self.led_index[l]
            for k in range(3):
                J[row + k, p0 + k] = 1
            row += 3
        for a, b in zip(self.w_i, self.w_j):
            J[row, base + 3 * a:base + 3 * a + 3] = 1
            J[row, base + 3 * b:base + 3 * b + 3] = 1
            row += 1
        return J

    # ---- the starting point ----
    def _stereo_points(self, s):
        """LEDs both cameras of station s saw, triangulated in its bottom camera's frame."""
        rig = self.rigs[self.station_rigs[s]]
        o = self.obs
        b = {l: k for k, l in zip(np.nonzero((o.station == s) & (o.cam == 0))[0],
                                   o.led[(o.station == s) & (o.cam == 0)])}
        tp = {l: k for k, l in zip(np.nonzero((o.station == s) & (o.cam == 1))[0],
                                    o.led[(o.station == s) & (o.cam == 1)])}
        common = sorted(set(b) & set(tp))
        if not common:
            return {}
        x1 = self.xn[[b[l] for l in common]].T
        x2 = self.xn[[tp[l] for l in common]].T
        Xh = cv2.triangulatePoints(np.hstack([np.eye(3), np.zeros((3, 1))]),
                                   np.hstack([rig.R_rel, rig.T_rel.reshape(3, 1)]), x1, x2)
        X = (Xh[:3] / Xh[3]).T
        return {l: X[k] for k, l in enumerate(common) if X[k, 2] > 0}

    def _pnp_station(self, s):
        """A station's pose from the anchors its cameras see (perspective-n-point)."""
        rig = self.rigs[self.station_rigs[s]]
        o = self.obs
        best = None
        for c in (0, 1):
            m = (o.station == s) & (o.cam == c) & np.isin(o.led, list(self.anchors))
            if m.sum() < 8:
                continue
            obj = np.array([self.anchors[l][0] for l in o.led[m]], dtype=np.float64)
            ok, rvec, tvec, inl = cv2.solvePnPRansac(obj, self.xn[m].reshape(-1, 1, 2), np.eye(3), None,
                                                     reprojectionError=3.0 / self.focal[m][0],
                                                     flags=cv2.SOLVEPNP_EPNP)
            if not ok or inl is None or (best is not None and len(inl) <= best[0]):
                continue
            Rcam, tcam = _rot(rvec), tvec.ravel()
            if c == 1:   # top camera -> the station's bottom-camera pose
                Rrel, Trel = rig.R_rel, rig.T_rel
                Rcam, tcam = Rrel.T @ Rcam, Rrel.T @ (tcam - Trel)
            best = (len(inl), Rcam, tcam)
        return None if best is None else best[1:]

    def _initial(self):
        stereo = [self._stereo_points(s) for s in range(self.n_st)]
        R = [None] * self.n_st
        t = [None] * self.n_st
        world = {}   # led -> list of world points (from placed stations)
        if self.anchors:
            for s in range(self.n_st):
                pose = self._pnp_station(s)
                if pose is None:
                    common = [l for l in stereo[s] if l in self.anchors]
                    if len(common) < 4:
                        continue
                    Rk, tk, _keep = _robust_kabsch(np.array([stereo[s][l] for l in common]),
                                                    np.array([self.anchors[l][0] for l in common]))
                    pose = (Rk.T, -Rk.T @ tk)    # world->bottom
                R[s], t[s] = pose
        else:
            R[0], t[0] = np.eye(3), np.zeros(3)
            self._fixed_R, self._fixed_t = R[0], t[0]
        for s in range(self.n_st):
            if R[s] is not None:
                for l, p in stereo[s].items():
                    world.setdefault(l, []).append(R[s].T @ (p - t[s]))
        # Chain the rest by their shared LEDs, most shared first.
        while any(r is None for r in R):
            best = None
            for s in range(self.n_st):
                if R[s] is not None:
                    continue
                common = [l for l in stereo[s] if l in world]
                if best is None or len(common) > len(best[1]):
                    best = (s, common)
            s, common = best
            if len(common) < 4:
                raise ValueError(f"station {s} shares too few LEDs with the others to be placed")
            P = np.array([stereo[s][l] for l in common])
            Q = np.array([np.mean(world[l], axis=0) for l in common])
            Rk, tk, _keep = _robust_kabsch(P, Q)       # bottom -> world
            R[s], t[s] = Rk.T, -Rk.T @ tk
            for l, p in stereo[s].items():
                world.setdefault(l, []).append(R[s].T @ (p - t[s]))
        # Every LED: linear triangulation over all its sightings.
        Rc, tc = self._camera_transforms(R, t, {n: np.zeros(3) for n in self.rig_names})
        X = np.zeros((len(self.leds), 3))
        o = self.obs
        for l in self.leds:
            m = np.nonzero(o.led == l)[0]
            if len(m) >= 2:
                A = []
                for i in m:
                    ci = 2 * o.station[i] + o.cam[i]
                    Pm = np.hstack([Rc[ci], tc[ci].reshape(3, 1)])
                    x, y = self.xn[i]
                    A.append(x * Pm[2] - Pm[0])
                    A.append(y * Pm[2] - Pm[1])
                _u, _s, Vt = np.linalg.svd(np.array(A))
                X[self.led_index[l]] = Vt[-1, :3] / Vt[-1, 3]
            elif l in world:
                X[self.led_index[l]] = np.mean(world[l], axis=0)
            if l in self.anchors:
                X[self.led_index[l]] = self.anchors[l][0]
        return R, t, X

    # ---- solving ----
    def solve(self, outlier_px=3.0, verbose=0):
        self._layout()
        R0, t0, X0 = self._initial()
        if self.fixed_station is not None:
            self._fixed_R, self._fixed_t = R0[self.fixed_station], t0[self.fixed_station]
        self.obs_pt = np.array([self.led_index[l] for l in self.obs.led])
        self.anchor_items = [(l, a) for l, a in self.anchors.items() if l in self.led_index]
        self.anchor_sigma = np.array([a[1] for _l, a in self.anchor_items]) if self.anchor_items else np.zeros(0)
        w = [(self.led_index[i], self.led_index[j], L) for i, j, L in self.wiring
             if i in self.led_index and j in self.led_index]
        self.w_i = np.array([a for a, _b, _L in w], dtype=np.int64)
        self.w_j = np.array([b for _a, b, _L in w], dtype=np.int64)
        self.w_max = np.array([L for _a, _b, L in w], dtype=np.float64)

        x = np.concatenate([np.concatenate([np.concatenate([cv2.Rodrigues(R0[s])[0].ravel(), t0[s]])
                                            for s in self.pose_stations]) if self.pose_stations else np.zeros(0),
                            np.zeros(self.n_rig), np.zeros(self.n_int), X0.ravel()])
        mask = np.ones(len(self.obs.led), bool)
        # A drop in front of the solve: sightings that can't be right even roughly.
        for rnd in range(3):
            sol = least_squares(self._residuals, x, args=(mask,), jac_sparsity=self._sparsity(mask),
                                loss="soft_l1", f_scale=1.0, x_scale="jac", method="trf",
                                max_nfev=200, verbose=verbose)
            x = sol.x
            r = self._residuals(x, np.ones(len(mask), bool))[:2 * len(mask)].reshape(-1, 2)
            err = np.linalg.norm(r, axis=1)
            new = err < outlier_px
            if (new == mask).all():
                break
            mask = new
        self.mask, self.x = mask, x
        return self._result(x, mask, err)

    def _result(self, x, mask, err):
        R, t, deltas, X = self._unpack(x)
        o = self.obs
        used = err[mask]
        rms = float(np.sqrt(np.mean(used ** 2))) if len(used) else 0.0
        intr = self._intrinsics(x)
        # Per-LED covariance, holding the poses: sigma^2 (J^T J)^-1 over its own sightings.
        Rc, tc = self._camera_transforms(R, t, deltas)
        cov, n_obs, n_st, points = {}, {}, {}, {}
        sigma = max(rms, 0.3)
        for l in self.leds:
            k = self.led_index[l]
            m = np.nonzero(mask & (o.led == l))[0]
            JtJ = np.zeros((3, 3))
            for i in m:
                ci = 2 * o.station[i] + o.cam[i]
                P = Rc[ci] @ X[k] + tc[ci]
                f = (intr[self.obs_cam[i], 0] + intr[self.obs_cam[i], 1]) / 2
                J = f * np.array([[1 / P[2], 0, -P[0] / P[2] ** 2], [0, 1 / P[2], -P[1] / P[2] ** 2]]) @ Rc[ci]
                JtJ += J.T @ J
            if l in self.anchors:
                JtJ += np.eye(3) / self.anchors[l][1] ** 2 * sigma ** 2
            points[l] = X[k]
            n_obs[l] = len(m)
            n_st[l] = len(set(o.station[m].tolist()))
            cov[l] = sigma ** 2 * np.linalg.pinv(JtJ) if len(m) >= 2 or l in self.anchors else np.full((3, 3), np.inf)
        return Result(points=points, cov=cov, n_obs=n_obs, n_stations=n_st, station_R=R, station_t=t,
                      rig_delta=deltas, rms_px=rms, dropped=int((~mask).sum()), residual_px=used,
                      intrinsics={k: intr[j] for j, k in enumerate(self.cam_keys)})


def wiring_limits(leds, lines=((0, 300), (300, 600), (600, 800), (800, 1000)), pitch_mm=100.0, joint_mm=300.0,
                  strand=100):
    """(i, i+1, max distance) for neighbouring LEDs on the same data line: the tree's
    wiring (docs: 100 mm within a strand, 300 mm across a strand joint)."""
    present = set(leds)
    out = []
    for a, b in lines:
        for i in range(a, b - 1):
            if i in present and i + 1 in present:
                out.append((i, i + 1, joint_mm if (i + 1) % strand == 0 else pitch_mm))
    return out
