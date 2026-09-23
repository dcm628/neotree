"""
Re-runs find_single_blob_centroid (current code, including any fix
being tested) against background/lit frame pairs already saved by
diagnose_blobs.py, so a detection-logic change can be verified against
frozen real captures instead of live hardware - removes trial-to-trial
signal variability from the comparison, isolating the effect of the
code change itself.

Usage (venv active, run from mapping/):
    python3 replay_blob_diag.py --dir /tmp/blob_diag_a --threshold 150
"""
import argparse
import glob
import os

import cv2

import neotree_camera as neocam


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--dir', required=True)
    parser.add_argument('--threshold', type=int, default=150)
    args = parser.parse_args()

    bg_files = sorted(glob.glob(os.path.join(args.dir, '*_background.jpg')))
    changed = 0
    for bg_path in bg_files:
        tag = os.path.basename(bg_path)[:-len('_background.jpg')]
        lit_path = os.path.join(args.dir, f"{tag}_lit.jpg")
        if not os.path.exists(lit_path):
            continue
        background = cv2.imread(bg_path)
        lit = cv2.imread(lit_path)
        cx, cy, blob_count, area = neocam.find_single_blob_centroid(
            lit, background=background, threshold_value=args.threshold)
        old_status = tag.rsplit('_', 1)[-1]
        new_status = "OK" if blob_count == 1 else ("ZERO" if blob_count == 0 else "AMBIG")
        marker = "  <-- CHANGED" if new_status != old_status else ""
        if marker:
            changed += 1
        print(f"{tag}: was {old_status} -> now {new_status} (blob_count={blob_count} area={area:.0f}){marker}")

    print(f"\n{changed}/{len(bg_files)} results changed by the current detection logic")


if __name__ == "__main__":
    main()
