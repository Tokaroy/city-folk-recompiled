"""In-between jumps in CF_INTERP_TEST=2 dumps (branch menu-scroll; docs/WII.md "Menu backgrounds at
high frame rates").

    py tools/interp/inbetween_jumps.py <dump dir> [--box x0,y0,x1,y1] [--ratio 1.5] [--min 1.0] [--delete]

A run with CF_INTERP_TEST=2 + CF_FRAME_DUMP=<dir>,<first>,<last> writes per game frame M
f<M>_s2.ppm (tape M as replayed: the game frame) and f<M>_s1.ppm (the in-between of M-1 and M at
alpha 0.5). An in-between belongs between its two game frames: its mean absolute difference to the
nearer of them is at most about their own difference. A jump (something drawn the long way round, at
another place, or another thing) differs from BOTH game frames by more than they differ from each
other. Per in-between: d_ab = D(M-1, M), d_mid = min(D(mid, M-1), D(mid, M)) (mean abs difference of
8-bit RGB inside --box, default the whole frame); flagged when d_mid > ratio x d_ab and d_mid > min.
Prints the flagged frames and a summary line; --delete removes the .ppm files afterwards.
"""
import argparse
import os
import re

import numpy as np
from PIL import Image


def load(path, box):
    im = np.asarray(Image.open(path).convert("RGB"), dtype=np.int16)
    if box:
        x0, y0, x1, y1 = box
        im = im[y0:y1, x0:x1]
    return im


def diff(a, b):
    return float(np.abs(a - b).mean())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--box", default="")
    ap.add_argument("--ratio", type=float, default=1.5)
    ap.add_argument("--min", type=float, default=1.0)
    ap.add_argument("--delete", action="store_true")
    ap.add_argument("--first", type=int, default=0)
    ap.add_argument("--last", type=int, default=1 << 30)
    args = ap.parse_args()
    box = tuple(int(v) for v in args.box.split(",")) if args.box else None
    frames = {}
    for n in os.listdir(args.dir):
        m = re.match(r"f(\d+)_s(\d)\.ppm$", n)
        if m:
            frames.setdefault(int(m.group(1)), {})[int(m.group(2))] = os.path.join(args.dir, n)
    flagged, total, worst = [], 0, (0.0, 0)
    for f in sorted(frames):
        if f < args.first or f > args.last:
            continue
        if 1 not in frames[f] or 2 not in frames[f] or f - 1 not in frames or 2 not in frames[f - 1]:
            continue
        a, b, mid = load(frames[f - 1][2], box), load(frames[f][2], box), load(frames[f][1], box)
        d_ab = diff(a, b)
        d_mid = min(diff(mid, a), diff(mid, b))
        total += 1
        r = d_mid / max(d_ab, 1e-3)
        if d_mid > args.min and r > args.ratio:
            flagged.append((f, d_ab, d_mid, r))
        if d_mid - d_ab > worst[0]:
            worst = (d_mid - d_ab, f)
    for f, d_ab, d_mid, r in flagged:
        print(f"jump f{f}: game-frame step {d_ab:.2f}, in-between off by {d_mid:.2f} (x{r:.1f})")
    print(f"{len(flagged)} jumps in {total} in-betweens (worst excess {worst[0]:.2f} at f{worst[1]})")
    if args.delete:
        for f in frames.values():
            for p in f.values():
                os.remove(p)
        for n in os.listdir(args.dir):
            if n.endswith("_fresh.ppm"):
                os.remove(os.path.join(args.dir, n))


if __name__ == "__main__":
    main()
