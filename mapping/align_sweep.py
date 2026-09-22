"""
Cross-sweep registration: aligns a new sweep's local-frame session_solves
onto the shared global frame (global_estimates) via weighted Kabsch +
RANSAC on LEDs solved both in this sweep and already in the global set -
the multi-pylon/multi-position mapping mechanism planned from the start
of this project's mapping design: treat every (sweep, pylon) as an
independent local point cloud, and align each new one against the
growing global estimate using shared, correctly-identified (by LED
index - no blind point matching needed, unlike generic point-cloud
registration) correspondences.

Bootstrapping: the very first sweep DEFINES the global frame by fiat (a
translation-only "transform", from the same coarse pylon->tree shift
already used ad hoc all night - see pylon_geometry.pylon_to_coarse_tree_frame)
since there's nothing yet to align against. Every subsequent sweep is
registered against whatever's already in global_estimates.

Usage (venv active):
    # first sweep - defines the global frame
    python3 align_sweep.py --bootstrap --sweep-id 2 --pylon-id A \\
        --standoff-mm 2220 --ground-offset-mm 880 --max-residual-mm 20

    # subsequent sweeps (after repositioning the pylon) - aligned onto
    # the existing global frame using whatever LEDs both sweeps solved
    python3 align_sweep.py --sweep-id 3 --pylon-id A --max-residual-mm 20
"""
import argparse

import numpy as np

import sweep_db


def kabsch(P, Q, weights=None):
    """
    Weighted Kabsch algorithm: best-fit rotation+translation mapping P
    onto Q (both (N,3)), minimizing sum(w_i * |R@P_i + t - Q_i|^2).
    Returns (R (3,3), t (3,)).
    """
    if weights is None:
        weights = np.ones(len(P))
    weights = np.asarray(weights, dtype=float)
    w_sum = weights.sum()
    p_bar = (weights[:, None] * P).sum(axis=0) / w_sum
    q_bar = (weights[:, None] * Q).sum(axis=0) / w_sum
    P_c = P - p_bar
    Q_c = Q - q_bar
    H = (weights[:, None] * P_c).T @ Q_c
    U, S, Vt = np.linalg.svd(H)
    d = np.sign(np.linalg.det(Vt.T @ U.T))
    D = np.diag([1.0, 1.0, d])
    R = Vt.T @ D @ U.T
    t = q_bar - R @ p_bar
    return R, t


def transform_error(R, t, P, Q):
    return np.linalg.norm((R @ P.T).T + t - Q, axis=1)


def ransac_align(P, Q, min_samples=3, iterations=1000, inlier_threshold_mm=50.0, rng=None):
    """
    Robust rigid alignment: repeatedly fit Kabsch on a random minimal
    subset, count inliers among all correspondences, keep the model with
    the most inliers, then refit Kabsch on all of that model's inliers.

    :return: (R, t, inlier_mask (N,) bool, rms_error_mm over the inliers)
    """
    rng = rng or np.random.default_rng()
    n = len(P)
    if n < min_samples:
        raise ValueError(f"need at least {min_samples} correspondences, got {n}")

    best_inliers = None
    best_count = -1
    for _ in range(iterations):
        idx = rng.choice(n, size=min_samples, replace=False)
        try:
            R, t = kabsch(P[idx], Q[idx])
        except np.linalg.LinAlgError:
            continue
        errors = transform_error(R, t, P, Q)
        inliers = errors < inlier_threshold_mm
        count = int(inliers.sum())
        if count > best_count:
            best_count = count
            best_inliers = inliers

    if best_inliers is None or best_inliers.sum() < min_samples:
        raise RuntimeError("RANSAC failed to find a usable inlier set - check inlier_threshold_mm "
                            "or whether the two sweeps actually share real overlap")

    R, t = kabsch(P[best_inliers], Q[best_inliers])
    final_errors = transform_error(R, t, P[best_inliers], Q[best_inliers])
    rms_error_mm = float(np.sqrt(np.mean(final_errors ** 2)))
    return R, t, best_inliers, rms_error_mm


def self_test():
    """Synthetic check: apply a known rotation+translation to a random
    point cloud (plus a few outliers), verify RANSAC recovers it."""
    rng = np.random.default_rng(42)
    n_good, n_outliers = 30, 6
    P = rng.uniform(-1000, 1000, size=(n_good, 3))
    true_rvec = np.array([0.1, -0.2, 0.3])
    import cv2
    true_R, _ = cv2.Rodrigues(true_rvec)
    true_t = np.array([500.0, -300.0, 200.0])
    Q = (true_R @ P.T).T + true_t

    P_outliers = rng.uniform(-1000, 1000, size=(n_outliers, 3))
    Q_outliers = rng.uniform(-1000, 1000, size=(n_outliers, 3))  # unrelated - true outliers
    P_all = np.vstack([P, P_outliers])
    Q_all = np.vstack([Q, Q_outliers])

    R_fit, t_fit, inliers, rms = ransac_align(P_all, Q_all, inlier_threshold_mm=1.0)
    print(f"true_R=\n{true_R.round(4)}\nfit_R=\n{R_fit.round(4)}")
    print(f"true_t={true_t}  fit_t={t_fit.round(3)}")
    print(f"inliers found: {inliers.sum()}/{len(P_all)} (expected {n_good})")
    print(f"rms_error={rms:.6f}mm")
    assert inliers.sum() == n_good, f"expected exactly the {n_good} real inliers, got {inliers.sum()}"
    assert np.allclose(R_fit, true_R, atol=1e-6), "rotation not recovered exactly"
    assert np.allclose(t_fit, true_t, atol=1e-6), "translation not recovered exactly"
    print("self-test passed: exact rotation/translation recovered, outliers correctly rejected")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--db', default=sweep_db.DEFAULT_DB_PATH)
    parser.add_argument('--sweep-id', required=True, type=int)
    parser.add_argument('--pylon-id', default='A')
    parser.add_argument('--max-residual-mm', type=float, default=None,
                         help="drop session_solves with ray_residual_mm above this before using them")
    parser.add_argument('--bootstrap', action='store_true',
                         help="this sweep DEFINES the global frame (coarse pylon->tree shift, no "
                              "alignment fit) instead of being registered against global_estimates")
    parser.add_argument('--standoff-mm', type=float, default=0.0, help="bootstrap only")
    parser.add_argument('--ground-offset-mm', type=float, default=0.0, help="bootstrap only")
    parser.add_argument('--inlier-threshold-mm', type=float, default=50.0)
    parser.add_argument('--ransac-iterations', type=int, default=1000)
    args = parser.parse_args()

    conn = sweep_db.connect(args.db)

    local_solves = {}  # led -> (point (3,), cov (3,3), residual)
    for led, x, y, z, cxx, cyy, czz, cxy, cxz, cyz, resid in conn.execute(
            "SELECT led_position, x_mm, y_mm, z_mm, cov_xx, cov_yy, cov_zz, cov_xy, cov_xz, cov_yz, "
            "ray_residual_mm FROM session_solves WHERE sweep_id = ? AND pylon_id = ?",
            (args.sweep_id, args.pylon_id)):
        if args.max_residual_mm is not None and resid > args.max_residual_mm:
            continue
        cov = np.array([[cxx, cxy, cxz], [cxy, cyy, cyz], [cxz, cyz, czz]])
        local_solves[led] = (np.array([x, y, z]), cov, resid)

    print(f"sweep {args.sweep_id} pylon {args.pylon_id}: {len(local_solves)} usable local solves "
          f"(residual filter: {args.max_residual_mm})")

    if args.bootstrap:
        R = np.eye(3)
        # same coarse shift as pylon_geometry.pylon_to_coarse_tree_frame,
        # expressed as a translation-only rigid transform
        t = np.array([0.0, -args.standoff_mm, args.ground_offset_mm])
        sweep_db.record_session_transform(
            conn, args.sweep_id, args.pylon_id, R, t,
            n_correspondences=len(local_solves), n_inliers=len(local_solves), rms_error_mm=0.0,
            reference_sweep_id=None, reference_pylon_id=None)
        print(f"Bootstrapped global frame from sweep {args.sweep_id} pylon {args.pylon_id} "
              f"(standoff={args.standoff_mm}mm ground_offset={args.ground_offset_mm}mm)")
    else:
        existing = {}
        for led, x, y, z in conn.execute("SELECT led_position, x_mm, y_mm, z_mm FROM global_estimates"):
            existing[led] = np.array([x, y, z])
        if not existing:
            raise SystemExit("global_estimates is empty - run with --bootstrap first on a reference sweep")

        shared = sorted(set(local_solves) & set(existing))
        print(f"{len(shared)} LEDs solved both in this sweep and the existing global frame")
        if len(shared) < 3:
            raise SystemExit(f"need at least 3 shared LEDs to align, got {len(shared)}")

        P = np.array([local_solves[led][0] for led in shared])
        Q = np.array([existing[led] for led in shared])
        R, t, inlier_mask, rms = ransac_align(
            P, Q, iterations=args.ransac_iterations, inlier_threshold_mm=args.inlier_threshold_mm)
        print(f"RANSAC: {int(inlier_mask.sum())}/{len(shared)} inliers, rms_error={rms:.1f}mm")
        sweep_db.record_session_transform(
            conn, args.sweep_id, args.pylon_id, R, t,
            n_correspondences=len(shared), n_inliers=int(inlier_mask.sum()), rms_error_mm=rms)

    R_final, t_final = sweep_db.get_session_transform(conn, args.sweep_id, args.pylon_id)
    added, merged = 0, 0
    for led, (point, cov, resid) in local_solves.items():
        global_point = R_final @ point + t_final
        global_cov = R_final @ cov @ R_final.T

        existing_row = conn.execute(
            "SELECT x_mm, y_mm, z_mm, cov_xx, cov_yy, cov_zz, contributing_sessions "
            "FROM global_estimates WHERE led_position = ?", (led,)).fetchone()
        if existing_row is None:
            sweep_db.set_global_estimate(conn, led, global_point, global_cov, 1)
            added += 1
        else:
            ex_point = np.array(existing_row[0:3])
            ex_cov = np.diag(existing_row[3:6])
            n_sessions = existing_row[6]
            w_existing = 1.0 / max(float(np.trace(ex_cov)), 1e-6)
            w_new = 1.0 / max(float(np.trace(global_cov)), 1e-6)
            combined_point = (w_existing * ex_point + w_new * global_point) / (w_existing + w_new)
            combined_cov = np.diag([1.0 / (w_existing + w_new)] * 3)
            sweep_db.set_global_estimate(conn, led, combined_point, combined_cov, n_sessions + 1)
            merged += 1

    conn.commit()
    total = conn.execute("SELECT COUNT(*) FROM global_estimates").fetchone()[0]
    conn.close()
    print(f"global_estimates: {added} new LEDs added, {merged} existing LEDs merged/updated "
          f"({total} total LEDs covered)")


if __name__ == "__main__":
    main()
