"""
Standard post-capture processing for a sweep: for each given pylon, fits
the top-camera tilt correction fresh (fit_camera_tilt.py - the rig is
deliberately hand-aimed rather than kept parallel each time, so this is
run every sweep, not reused from a prior one) and re-triangulates that
pylon's session_solves using it (retriangulate_sweep.py). Run this right
after every capture_sweep.py run, for every pylon that was active.

Usage (venv active):
    python3 process_sweep.py --sweep-id 3 --pylon-id A --pylon-id B
"""
import argparse
import subprocess
import sys

import sweep_db


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--db', default=sweep_db.DEFAULT_DB_PATH)
    parser.add_argument('--sweep-id', required=True, type=int)
    parser.add_argument('--pylon-id', action='append', required=True, dest='pylon_ids',
                         help="repeatable - one --pylon-id per active pylon, e.g. --pylon-id A --pylon-id B")
    parser.add_argument('--spacing-mm', type=float, default=None,
                         help="override pylon_geometry.PYLON_CAMERA_SPACING_MM for all pylons "
                              "(same value used for every pylon given - pass separately per "
                              "pylon by running this script once per pylon if they differ)")
    args = parser.parse_args()

    for pylon_id in args.pylon_ids:
        print(f"\n=== pylon {pylon_id} ===")
        fit_cmd = [sys.executable, "fit_camera_tilt.py", "--db", args.db,
                   "--sweep-id", str(args.sweep_id), "--pylon-id", pylon_id]
        retri_cmd = [sys.executable, "retriangulate_sweep.py", "--db", args.db,
                     "--sweep-id", str(args.sweep_id), "--pylon-id", pylon_id]
        if args.spacing_mm is not None:
            fit_cmd += ["--spacing-mm", str(args.spacing_mm)]
            retri_cmd += ["--spacing-mm", str(args.spacing_mm)]

        subprocess.run(fit_cmd, check=True)
        subprocess.run(retri_cmd, check=True)


if __name__ == "__main__":
    main()
