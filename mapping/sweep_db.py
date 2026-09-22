"""
SQLite storage for the multi-pylon mapping pipeline.

Design principle: raw_observations is the source of truth and is never
mutated once written - every centroid capture attempt is logged, including
"not found" ones. session_solves (per-pylon-per-sweep triangulated 3D
points) is a *derived* table, fully regeneratable from raw_observations by
re-running triangulation; it's stored rather than recomputed on the fly
only so results survive between runs and don't need re-triangulating on
every query. Cross-session alignment (registering each session into one
shared frame) and the final merged per-LED global estimate are a later
phase and intentionally have no tables yet - adding them now would be
schema for an algorithm that doesn't exist.
"""
import sqlite3
from contextlib import closing
from datetime import datetime, timezone

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
