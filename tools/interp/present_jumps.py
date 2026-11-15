"""Jumps in a sequence of present dumps (branch menu-scroll; docs/WII.md "Menu backgrounds at high
frame rates").

    py tools/interp/present_jumps.py <dump dir> [--box x0,y0,x1,y1] [--ratio 5] [--min 0.5] [--delete]

A run with CF_PRESENT_DUMP=1 + CF_FRAME_DUMP=<dir>,<first>,<last> writes every present of the tapes
in the range as f<frame>_p<present>_a<alpha>.ppm (real time). The step between consecutive presents
(mean abs difference of 8-bit RGB inside --box) is steady while things move evenly; a jump (a picture
somewhere else for one interval: 1 present at 60, 2 at 120, 4 at 240, then back) shows as a spike
there and one back: a step over --ratio x the median step of the presents around it (+-8) and over
--min. Counted as jumps = spikes (each jump gives two). Also lists repeated presents.
"""
import argparse
import os
import re

import numpy as np
from PIL import Image


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--box", default="")
    ap.add_argument("--ratio", type=float, default=5.0)
    ap.add_argument("--min", type=float, default=0.5)
    ap.add_argument("--delete", action="store_true")
    args = ap.parse_args()
    box = tuple(int(v) for v in args.box.split(",")) if args.box else None
    items = []
    for n in os.listdir(args.dir):
        m = re.match(r"f(\d+)_p(\d+)_a(\d+)\.ppm$", n)
        if m:
            items.append((int(m.group(2)), int(m.group(1)), int(m.group(3)), os.path.join(args.dir, n)))
    items.sort()

    def load(p):
        im = np.asarray(Image.open(p).convert("RGB"), dtype=np.int16)
        if box:
            im = im[box[1]:box[3], box[0]:box[2]]
        return im

    def diff(a, b):
        return float(np.abs(a - b).mean())

    steps, repeats = [], 0
    prev = None
    for pres, frame, alpha, path in items:
        cur = load(path)
        if prev is not None and prev[0] + 1 == pres:
            d = diff(prev[1], cur)
            steps.append((pres, frame, alpha, d))
            if d == 0.0:
                repeats += 1
        prev = (pres, cur)
    spikes = []
    for i, (pres, frame, alpha, d) in enumerate(steps):
        around = sorted(x[3] for x in steps[max(0, i - 8):i + 9] if x is not steps[i])
        med = around[len(around) // 2] if around else 0.0
        if d > args.min and d > args.ratio * max(med, 1e-3):
            spikes.append((pres, frame, alpha, d, med))
    for pres, frame, alpha, d, med in spikes:
        print(f"spike into present {pres} (f{frame} a{alpha / 1000:.3f}): step {d:.2f}, median around {med:.2f}")
    print(f"{len(spikes)} spikes in {len(steps)} present steps, {repeats} repeated presents")
    if args.delete:
        for it in items:
            os.remove(it[3])


if __name__ == "__main__":
    main()
