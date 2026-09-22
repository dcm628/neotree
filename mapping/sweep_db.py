"""
SQLite storage for the multi-pylon mapping pipeline.

Design principle: raw_observations is the source of truth and is never
mutated once written - every centroid capture attempt is logged, including
"not found" ones. session_solves (per-pylon-per-sweep triangulated 3D
points) is a *derived* table, fully regeneratable from raw_observations by
re-running triangulation; it's stored rather than recomputed on the fly
only so results survive between runs and don't need re-triangulating on
every query. session_transforms (the rigid transform aligning one
session onto the shared global frame) and global_estimates (the final
per-LED merged position) are likewise derived/regeneratable from
session_solves - see align_sweep.py.
"""
import json
import sqlite3
from contextlib import closing
from datetime import datetime, timezone

import numpy as np

DEFAULT_DB_PATH = "sweep_data.sqlite3"

SCHEMA = """
CREATE TABLE IF NOT EXISTS sweeps (
    sweep_id INTEGER PRIMARY KEY AUTOINCREMENT,
    started_at TEXT NOT NULL,
    notes TEXT
);

CREATE TABLE IF NOT EXISTS pylon_placements (
    sweep_id INTEGER NOT NULL REFERENCES sweeps(sweep_id),
    pylon_id TEXT NOT NULL,
    top_camera_id INTEGER NOT NULL,
    bottom_camera_id INTEGER NOT NULL,
    capture_width INTEGER NOT NULL,
    capture_height INTEGER NOT NULL,
    camera_spacing_mm REAL NOT NULL,
    calibration_source TEXT NOT NULL DEFAULT 'nominal',
    PRIMARY KEY (sweep_id, pylon_id)
);

CREATE TABLE IF NOT EXISTS raw_observations (
    sweep_id INTEGER NOT NULL,
    pylon_id TEXT NOT NULL,
    camera_position TEXT NOT NULL CHECK (camera_position IN ('top', 'bottom')),
    led_position INTEGER NOT NULL,
    found INTEGER NOT NULL,
    pixel_x REAL,
    pixel_y REAL,
    blob_count INTEGER,
    blob_area REAL,
    attempts INTEGER NOT NULL,
    captured_at TEXT NOT NULL,
    PRIMARY KEY (sweep_id, pylon_id, camera_position, led_position),
    FOREIGN KEY (sweep_id, pylon_id) REFERENCES pylon_placements(sweep_id, pylon_id)
);

CREATE TABLE IF NOT EXISTS session_solves (
    sweep_id INTEGER NOT NULL,
    pylon_id TEXT NOT NULL,
    led_position INTEGER NOT NULL,
    x_mm REAL NOT NULL,
    y_mm REAL NOT NULL,
    z_mm REAL NOT NULL,
    cov_xx REAL NOT NULL, cov_yy REAL NOT NULL, cov_zz REAL NOT NULL,
    cov_xy REAL NOT NULL, cov_xz REAL NOT NULL, cov_yz REAL NOT NULL,
    ray_residual_mm REAL NOT NULL,
    n_rays INTEGER NOT NULL,
    solved_at TEXT NOT NULL,
    PRIMARY KEY (sweep_id, pylon_id, led_position),
    FOREIGN KEY (sweep_id, pylon_id) REFERENCES pylon_placements(sweep_id, pylon_id)
);

-- The rigid transform (rotation + translation) that maps one (sweep_id,
-- pylon_id) session's local-frame session_solves onto the shared global
-- frame - fitted by align_sweep.py via weighted Kabsch + RANSAC on LEDs
-- solved both in this session and already in global_estimates.
-- reference_sweep_id/reference_pylon_id record what it was aligned onto;
-- NULL for the first session, which defines the global frame by fiat
-- (identity transform).
CREATE TABLE IF NOT EXISTS session_transforms (
    sweep_id INTEGER NOT NULL,
    pylon_id TEXT NOT NULL,
    rotation_json TEXT NOT NULL,   -- 3x3 matrix, row-major, JSON list of 9 floats
    translation_x REAL NOT NULL,
    translation_y REAL NOT NULL,
    translation_z REAL NOT NULL,
    n_correspondences INTEGER NOT NULL,
    n_inliers INTEGER NOT NULL,
    rms_error_mm REAL NOT NULL,
    reference_sweep_id INTEGER,
    reference_pylon_id TEXT,
    fitted_at TEXT NOT NULL,
    PRIMARY KEY (sweep_id, pylon_id)
);

-- Final per-LED merged estimate in the shared global frame - derived from
-- every session_solves entry once transformed by its session_transforms
-- row, confidence-weighted (by inverse ray_residual_mm) across sessions
-- that solved the same LED. Regeneratable from session_solves +
-- session_transforms; stored so downstream tools (generate_pos_config_
-- header.py) don't need to redo the merge on every run.
CREATE TABLE IF NOT EXISTS global_estimates (
    led_position INTEGER PRIMARY KEY,
    x_mm REAL NOT NULL,
    y_mm REAL NOT NULL,
    z_mm REAL NOT NULL,
    cov_xx REAL NOT NULL, cov_yy REAL NOT NULL, cov_zz REAL NOT NULL,
    contributing_sessions INTEGER NOT NULL,
    updated_at TEXT NOT NULL
);
"""


def now_iso():
    return datetime.now(timezone.utc).isoformat()


def connect(db_path=DEFAULT_DB_PATH):
    conn = sqlite3.connect(db_path)
    conn.execute("PRAGMA foreign_keys = ON")
    conn.executescript(SCHEMA)
    return conn


def start_sweep(conn, notes=None):
    with conn:
        cur = conn.execute("INSERT INTO sweeps (started_at, notes) VALUES (?, ?)",
                            (now_iso(), notes))
        return cur.lastrowid


def record_pylon_placement(conn, sweep_id, pylon_id, top_camera_id, bottom_camera_id,
                            capture_width, capture_height, camera_spacing_mm,
                            calibration_source="nominal"):
    with conn:
        conn.execute(
            "INSERT INTO pylon_placements "
            "(sweep_id, pylon_id, top_camera_id, bottom_camera_id, capture_width, "
            " capture_height, camera_spacing_mm, calibration_source) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
            (sweep_id, pylon_id, top_camera_id, bottom_camera_id, capture_width,
             capture_height, camera_spacing_mm, calibration_source))


def record_raw_observation(conn, sweep_id, pylon_id, camera_position, led_position,
                            found, pixel_x=None, pixel_y=None, blob_count=None,
                            blob_area=None, attempts=1):
    with conn:
        conn.execute(
            "INSERT OR REPLACE INTO raw_observations "
            "(sweep_id, pylon_id, camera_position, led_position, found, pixel_x, "
            " pixel_y, blob_count, blob_area, attempts, captured_at) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
            (sweep_id, pylon_id, camera_position, led_position, int(found), pixel_x,
             pixel_y, blob_count, blob_area, attempts, now_iso()))


def record_session_solve(conn, sweep_id, pylon_id, led_position, result):
    """
    :param result: a pylon_geometry.TriangulationResult.
    """
    cov = result.covariance
    with conn:
        conn.execute(
            "INSERT OR REPLACE INTO session_solves "
            "(sweep_id, pylon_id, led_position, x_mm, y_mm, z_mm, "
            " cov_xx, cov_yy, cov_zz, cov_xy, cov_xz, cov_yz, "
            " ray_residual_mm, n_rays, solved_at) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
            (sweep_id, pylon_id, led_position,
             float(result.point[0]), float(result.point[1]), float(result.point[2]),
             float(cov[0, 0]), float(cov[1, 1]), float(cov[2, 2]),
             float(cov[0, 1]), float(cov[0, 2]), float(cov[1, 2]),
             result.ray_residual_mm, result.n_rays, now_iso()))


def record_session_transform(conn, sweep_id, pylon_id, rotation, translation,
                              n_correspondences, n_inliers, rms_error_mm,
                              reference_sweep_id=None, reference_pylon_id=None):
    """
    :param rotation: (3,3) array-like.
    :param translation: (3,) array-like.
    """
    rotation = np.asarray(rotation, dtype=float)
    translation = np.asarray(translation, dtype=float)
    with conn:
        conn.execute(
            "INSERT OR REPLACE INTO session_transforms "
            "(sweep_id, pylon_id, rotation_json, translation_x, translation_y, translation_z, "
            " n_correspondences, n_inliers, rms_error_mm, reference_sweep_id, reference_pylon_id, fitted_at) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
            (sweep_id, pylon_id, json.dumps(rotation.flatten().tolist()),
             float(translation[0]), float(translation[1]), float(translation[2]),
             n_correspondences, n_inliers, rms_error_mm,
             reference_sweep_id, reference_pylon_id, now_iso()))


def get_session_transform(conn, sweep_id, pylon_id):
    """Returns (rotation (3,3) ndarray, translation (3,) ndarray) or None if not fitted yet."""
    row = conn.execute(
        "SELECT rotation_json, translation_x, translation_y, translation_z "
        "FROM session_transforms WHERE sweep_id = ? AND pylon_id = ?", (sweep_id, pylon_id)).fetchone()
    if row is None:
        return None
    rotation = np.array(json.loads(row[0])).reshape(3, 3)
    translation = np.array(row[1:4])
    return rotation, translation


def set_global_estimate(conn, led_position, point, covariance, contributing_sessions):
    with conn:
        conn.execute(
            "INSERT OR REPLACE INTO global_estimates "
            "(led_position, x_mm, y_mm, z_mm, cov_xx, cov_yy, cov_zz, contributing_sessions, updated_at) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
            (led_position, float(point[0]), float(point[1]), float(point[2]),
             float(covariance[0, 0]), float(covariance[1, 1]), float(covariance[2, 2]),
             contributing_sessions, now_iso()))


def sweep_summary(conn, sweep_id):
    """
    Per-pylon counts of solved / partial (one camera only) / unfound LEDs
    for one sweep, for the end-of-sweep report.
    """
    with closing(conn.cursor()) as cur:
        pylons = [row[0] for row in cur.execute(
            "SELECT pylon_id FROM pylon_placements WHERE sweep_id = ?", (sweep_id,))]

        summary = {}
        for pylon_id in pylons:
            solved = cur.execute(
                "SELECT COUNT(*) FROM session_solves WHERE sweep_id = ? AND pylon_id = ?",
                (sweep_id, pylon_id)).fetchone()[0]
            one_camera = cur.execute(
                "SELECT COUNT(*) FROM ("
                " SELECT led_position FROM raw_observations "
                " WHERE sweep_id = ? AND pylon_id = ? AND found = 1 "
                " GROUP BY led_position HAVING COUNT(*) = 1"
                ")", (sweep_id, pylon_id)).fetchone()[0]
            none_found = cur.execute(
                "SELECT COUNT(*) FROM ("
                " SELECT led_position FROM raw_observations "
                " WHERE sweep_id = ? AND pylon_id = ? "
                " GROUP BY led_position HAVING SUM(found) = 0"
                ")", (sweep_id, pylon_id)).fetchone()[0]
            summary[pylon_id] = {
                "solved": solved,
                "partial_one_camera": one_camera,
                "no_centroid": none_found,
            }
        return summary
