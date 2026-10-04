"""
Synthetic rehearsal of the phased mapping plan (docs/CALIBRATION.md): a tree
with the real profile, LEDs strung as wire chains (100 mm pitch, wrapping
the branches), two pylons placed at six stations, branches hiding LEDs,
0.3 px pixel noise and 2% bad blobs, and board calibrations carrying the
errors seen in test_calibration_synthetic.py. Measures each result against
the truth:

  - one station's own stereo, as the old pipeline would;
  - phase 1: the top section, close range, bundle-adjusted across stations;
  - phase 2: the whole tree from 2.3 m - alone, and anchored on phase 1.

Run on the Pi (venv active, from mapping/): python3 test_bundle_synthetic.py
"""
import sys

import cv2
import numpy as np

import bundle

rng = np.random.default_rng(int(sys.argv[1]) if len(sys.argv) > 1 else 11)
REFINE_INTRINSICS = "--fixed-intrinsics" not in sys.argv
W, H = 1280, 720

PROFILE = [(-120, 490), (100, 350), (300, 450), (500, 530), (700, 510), (900, 420), (1100, 380),
           (1300, 336), (1500, 320), (1700, 295), (1900, 275), (2000, 200)]


def radius_at(z):
    zs, rs = zip(*PROFILE)
    return float(np.interp(z, zs, rs))


LINES = (300, 300, 200, 200)      # the real tree: data lines of 100-LED strands


def make_tree(lines=LINES):
    """LEDs along wire chains: each step 60-100 mm (up to 280 mm across a strand joint),
    mostly around the tree, drifting in height, in and out along branches - inside the
    tree's profile."""
    leds = []
    for line, per_line in enumerate(lines):
        upward = line % 2 == 0                # each string climbs or descends the whole tree
        z = -80.0 if upward else 1990.0
        drift = 2050.0 / per_line * (1 if upward else -1)
        ang = rng.uniform(0, 2 * np.pi)
        frac = rng.uniform(0.5, 1.0)
        for k in range(per_line):
            r = radius_at(z) * frac
            p = np.array([r * np.cos(ang), r * np.sin(ang), z])
            limit = 280.0 if k and k % 100 == 0 else 98.0      # the wire: 100 mm, 300 at a strand joint
            if k and np.linalg.norm(p - leds[-1]) > limit:
                p = leds[-1] + (p - leds[-1]) * (limit / np.linalg.norm(p - leds[-1]))
                z, ang = p[2], np.arctan2(p[1], p[0])
                frac = float(np.clip(np.hypot(p[0], p[1]) / radius_at(z), 0.25, 1.0))
            leds.append(p)
            step = rng.uniform(60, 100)
            dz = rng.normal(drift, 25)
            frac = float(np.clip(frac + rng.normal(0, 0.12), 0.25, 1.0))
            horiz = np.sqrt(max(step ** 2 - dz ** 2, 100.0))
            ang += horiz / max(r, 80.0) * rng.choice([1, 1, 1, -1])
            z = float(np.clip(z + dz, -100, 2000))
    leds = np.array(leds)
    return leds


def camera_pose(center, aim):
    """World->camera (OpenCV) for a portrait-mounted camera: raw x = up, raw y = right."""
    f = aim - center
    f /= np.linalg.norm(f)
    up = np.array([0.0, 0.0, 1.0]) - f[2] * f
    up /= np.linalg.norm(up)
    right = np.cross(f, up)
    R = np.vstack([up, right, f])
    return R, -R @ center


def true_camera():
    K = np.array([[rng.normal(935, 10), 0, rng.normal(640, 6)], [0, 0, rng.normal(360, 6)], [0, 0, 1]])
    K[1, 1] = K[0, 0] * rng.normal(1.0, 0.003)
    D = np.array([rng.normal(0.1, 0.02), rng.normal(-0.23, 0.04), rng.normal(0, 0.001), rng.normal(0, 0.001),
                  rng.normal(0.13, 0.03)])
    return K, D


def calibrated(K, D):
    """What a board calibration returns: close, not exact."""
    Kc = K.copy()
    Kc[0, 0] *= 1 + rng.normal(0, 0.002)
    Kc[1, 1] *= 1 + rng.normal(0, 0.002)
    Kc[0, 2] += rng.normal(0, 1.5)
    Kc[1, 2] += rng.normal(0, 1.5)
    return Kc, D + rng.normal(0, [0.003, 0.008, 0.0003, 0.0003, 0.008])


def build_phase(leds, standoff, cam_z_bottom, aim_z, spacing, rig_cams, stations_deg, mrad=0.5, base_err_mm=1.0):
    """Stations (2 pylons alternating), true poses, observations, and the calibrated rigs."""
    rigs, station_rigs, rows = {}, [], []
    truth_poses = []
    rig_names = ["A", "B"]
    # Each rig's true stereo (from how it's aimed) and its board calibration.
    rig_truth = {}
    for name in rig_names:
        (Kb, Db), (Kt, Dt) = rig_cams[name]
        cb = np.array([standoff, 0.0, cam_z_bottom])
        ct = cb + np.array([rng.normal(0, 3), rng.normal(0, 3), spacing])
        aim = np.array([0.0, 0.0, aim_z])
        Rb, tb = camera_pose(cb, aim + rng.normal(0, 15, 3))
        Rt, tt = camera_pose(ct, aim + rng.normal(0, 15, 3))
        R_rel = Rt @ Rb.T
        T_rel = tt - R_rel @ tb
        rig_truth[name] = (Rb, tb, Rt, tt)
        Kbc, Dbc = calibrated(Kb, Db)
        Ktc, Dtc = calibrated(Kt, Dt)
        R_err = cv2.Rodrigues(rng.normal(0, mrad / 1000.0, 3))[0]
        T_err = T_rel + rng.normal(0, base_err_mm, 3)
        rigs[name] = bundle.Rig(name, Kbc, Dbc, Ktc, Dtc, R_err @ R_rel, T_err)
    for k, deg in enumerate(stations_deg):
        name = rig_names[k % 2]
        a = np.radians(deg)
        Rz = np.array([[np.cos(a), -np.sin(a), 0], [np.sin(a), np.cos(a), 0], [0, 0, 1]])
        Rb, tb, Rt, tt = rig_truth[name]
        # The rig carried round the tree: rotate it about the trunk.
        Rb_s, tb_s = Rb @ Rz.T, tb
        Rt_s, tt_s = Rt @ Rz.T, tt
        station_rigs.append(name)
        truth_poses.append((Rb_s, tb_s))
        cams = [(Rb_s, tb_s) + rig_cams[name][0], (Rt_s, tt_s) + rig_cams[name][1]]
        station_dir = np.array([np.cos(a), np.sin(a)])
        hidden = rng.random(len(leds)) < 0.2            # branches in the way, for this station
        for c, (R, t, K, D) in enumerate(cams):
            P = leds @ R.T + t
            uv = cv2.projectPoints(leds, cv2.Rodrigues(R)[0], t, K, D)[0].reshape(-1, 2)
            for i, (p, (u, v)) in enumerate(zip(P, uv)):
                if p[2] <= 0 or not (5 < u < W - 5 and 5 < v < H - 5) or hidden[i] or rng.random() < 0.08:
                    continue
                r = np.hypot(*leds[i, :2])
                facing = np.dot(leds[i, :2] / max(r, 1e-6), station_dir)
                depth_frac = r / radius_at(leds[i, 2])
                if facing < -0.1 - (1 - depth_frac) * 1.2:   # behind the tree: hidden (less so for inner LEDs)
                    continue
                u, v = u + rng.normal(0, 0.3), v + rng.normal(0, 0.3)
                if rng.random() < 0.02:                      # a bad blob: a reflection or a neighbour
                    u, v = u + rng.uniform(4, 40) * rng.choice([-1, 1]), v + rng.uniform(4, 40) * rng.choice([-1, 1])
                rows.append((k, c, i, u, v))
    return rigs, station_rigs, bundle.Observations.from_rows(rows), truth_poses


def errors_after_rigid(points, truth, leds_subset=None):
    keys = [l for l in points if leds_subset is None or l in leds_subset]
    P = np.array([points[l] for l in keys])
    Q = truth[keys]
    R, t, keep = bundle._robust_kabsch(P, Q, inlier_mm=50)
    e = np.linalg.norm(P @ R.T + t - Q, axis=1)
    return e, (R, t)


def summary(label, e):
    print(f"  {label}: {len(e)} LEDs, error median {np.median(e):.1f} mm, 90% {np.percentile(e, 90):.1f} mm, "
          f"95% {np.percentile(e, 95):.1f} mm")


def main():
    leds = make_tree()
    top = set(np.nonzero(leds[:, 2] >= 1200)[0].tolist())
    starts = np.cumsum((0,) + LINES)[:-1]
    steps = np.linalg.norm(np.diff(leds, axis=0), axis=1)[[k for k in range(len(leds) - 1)
                                                           if (k + 1) not in starts and (k + 1) % 100]]
    print(f"tree: {len(leds)} LEDs, {len(top)} in the top section (z >= 1200 mm); "
          f"neighbours {steps.min():.0f}-{steps.max():.0f} mm apart")
    stations = [0, 180, 60, 240, 120, 300]    # sweeps: A+B opposite each other

    # ---- phase 1: the top section from 1.1 m ----
    cams1 = {n: (true_camera(), true_camera()) for n in "AB"}
    rigs1, st1, obs1, _tp1 = build_phase(leds, 1100, 1304, 1600, 592, cams1, stations)
    keep = np.isin(obs1.led, list(top)) | True
    print(f"\nphase 1: {len(obs1.led)} sightings from {len(st1)} stations")

    # One station's own stereo, as before.
    b1 = bundle.Bundle(rigs1, st1, obs1)
    b1._layout()
    best = None
    for s in range(len(st1)):
        sp = b1._stereo_points(s)
        sp = {l: p for l, p in sp.items() if l in top}
        if len(sp) > 30:
            e, _ = errors_after_rigid(sp, leds)
            if best is None or np.median(e) < np.median(best):
                best = e
    summary("best single station, its own stereo", best)

    res1 = bundle.Bundle(rigs1, st1, obs1, refine_rigs=True, refine_intrinsics=REFINE_INTRINSICS).solve()
    print(f"  bundle: rms {res1.rms_px:.2f} px, {res1.dropped} sightings rejected as bad blobs")
    e1, (R1, t1) = errors_after_rigid(res1.points, leds, top)
    summary("phase 1 bundle, top section", e1)
    sig = {l: np.sqrt(np.trace(res1.cov[l])) for l in res1.points}
    chosen = bundle.choose_anchors(res1, bundle.wiring_limits(res1.points))
    anchors = {l: (res1.points[l], max(sig[l], 0.5)) for l in chosen}
    ea = np.array([np.linalg.norm(R1 @ res1.points[l] + t1 - leds[l]) for l in anchors])
    print(f"  anchors chosen: {len(anchors)}; their true error median {np.median(ea):.1f} mm, "
          f"max {ea.max():.1f} mm; estimated median {np.median([anchors[l][1] for l in anchors]):.1f} mm")

    # ---- phase 2: the whole tree from 2.3 m ----
    cams2 = {n: (true_camera(), true_camera()) for n in "AB"}      # refocused: new intrinsics
    rigs2, st2, obs2, _tp2 = build_phase(leds, 2300, 644, 940, 592, cams2, stations)
    print(f"\nphase 2: {len(obs2.led)} sightings from {len(st2)} stations")
    b2 = bundle.Bundle(rigs2, st2, obs2)
    b2._layout()
    best = None
    for s in range(len(st2)):
        sp = b2._stereo_points(s)
        if len(sp) > 50:
            e, _ = errors_after_rigid(sp, leds)
            if best is None or np.median(e) < np.median(best):
                best = e
    summary("best single station, its own stereo", best)

    res2 = bundle.Bundle(rigs2, st2, obs2, refine_rigs=True).solve()
    e2, _ = errors_after_rigid(res2.points, leds)
    summary("phase 2 bundle without anchors", e2)

    wiring = bundle.wiring_limits(range(len(leds)))
    res3 = bundle.Bundle(rigs2, st2, obs2, refine_rigs=True, anchors=anchors, wiring=wiring,
                         refine_intrinsics=REFINE_INTRINSICS).solve()
    print(f"  anchored bundle: rms {res3.rms_px:.2f} px, {res3.dropped} rejected")
    # Anchored results are in phase 1's frame: map to the truth with phase 1's alignment (no refit).
    e3 = np.array([np.linalg.norm(R1 @ res3.points[l] + t1 - leds[l]) for l in res3.points])
    summary("phase 2 anchored on phase 1, whole tree (in phase 1's frame, no refit)", e3)
    lower = [l for l in res3.points if l not in top]
    e3l = np.array([np.linalg.norm(R1 @ res3.points[l] + t1 - leds[l]) for l in lower])
    summary("  of which below the top section", e3l)
    n2 = np.array([res3.n_stations[l] for l in res3.points])
    print(f"  LEDs seen by 1 station: {(n2 == 1).sum()}, 2: {(n2 == 2).sum()}, 3+: {(n2 >= 3).sum()}, "
          f"none: {len(leds) - len(res3.points)}")
    broken = sum(1 for i, j, L in wiring if i in res3.points and j in res3.points
                 and np.linalg.norm(res3.points[i] - res3.points[j]) > L + 10)
    print(f"  wiring limits broken by more than 10 mm: {broken}")


if __name__ == "__main__":
    main()
