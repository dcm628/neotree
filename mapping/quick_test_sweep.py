"""
Short, targeted validation sweep over a small LED range in the middle of
the known-strung string (default: 650-679, the midpoint of the 300-999
strung range) - for quickly checking viability (lighting conditions,
camera settings, rig aim) without committing to a full multi-hundred-LED,
multi-minute sweep. A thin wrapper around capture_sweep.py with shorter
defaults; every capture_sweep.py option can still be overridden.

Born from a real need: daytime ambient light through windows required
re-tuning (per-LED fresh background frames instead of one captured at
sweep start, and a much lower detection threshold - the LED's own
contribution above the ambient background was only ~60-70 brightness
levels, nowhere near the ~250 that worked with a dark room and zero
ambient light). A short sweep like this is the fast way to check any of
that is actually working before running a real 700-LED sweep and finding
out 15 minutes later that it wasn't.

Usage (venv active):
    python3 quick_test_sweep.py --pylon-a-top 0 --pylon-a-bottom 2 \\
        --pylon-b-top 4 --pylon-b-bottom 6 --threshold 150
    python3 quick_test_sweep.py --pylon-a-top 0 --pylon-a-bottom 2 \\
        --start-led 800 --count 15   # override the range too
"""
import argparse
import sys

import capture_sweep


def build_parser():
    parser = argparse.ArgumentParser(description=__doc__,
                                      formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--pylon-a-top', type=int)
    parser.add_argument('--pylon-a-bottom', type=int)
    parser.add_argument('--pylon-b-top', type=int)
    parser.add_argument('--pylon-b-bottom', type=int)
    parser.add_argument('--db', default=None)
    parser.add_argument('--width', type=int, default=None)
    parser.add_argument('--height', type=int, default=None)
    parser.add_argument('--spacing-mm', type=float, default=None)
    parser.add_argument('--start-led', type=int, default=650,
                         help="default 650 - the midpoint of the 300-999 known-strung range")
    parser.add_argument('--count', type=int, default=30, help="default 30 - short, for a quick check")
    parser.add_argument('--dwell', type=float, default=None)
    parser.add_argument('--background-dwell', type=float, default=None)
    parser.add_argument('--retries', type=int, default=None)
    parser.add_argument('--threshold', type=int, default=150,
                         help="default 150, lower than capture_sweep.py's 250 default - tuned for "
                              "daylight ambient conditions where the LED's own contribution above "
                              "background is much smaller than in a dark room; use 250 (or pass it "
                              "explicitly) for night/dark-room sweeps")
    parser.add_argument('--pixel-sigma', type=float, default=None)
    parser.add_argument('--notes', default="quick test sweep")
    return parser


def main():
    args = build_parser().parse_args()

    forwarded = []
    for key, value in vars(args).items():
        if value is None:
            continue
        forwarded.extend(['--' + key.replace('_', '-'), str(value)])

    sys.argv = ['capture_sweep.py'] + forwarded
    capture_sweep.main()


if __name__ == "__main__":
    main()
