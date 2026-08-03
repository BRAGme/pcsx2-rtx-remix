"""Where do two captures differ? Saves a mask so the changed region can be identified by eye.

STABLEID=1 did not reduce the frame-to-frame Geometry Hash variation (15.69 vs 15.32 /255) even
though it cut mesh creations 7,258 -> 4,255. So the ~18% of pixels that change every frame while
the camera is stationary are probably not identity churn at all. The obvious alternative is
genuinely animated geometry -- the player's weapon and the animated teammate -- which SHOULD
re-hash every frame. This localizes the change to settle that.

White = changed beyond threshold, black = unchanged. Same top-10% crop as colmetric.py.

Usage: python diffmask.py <a.png> <b.png> <out.png> [threshold]
"""

import sys
from PIL import Image
import numpy as np


def load(path):
    im = Image.open(path).convert("RGB")
    w, h = im.size
    return im.crop((0, int(h * 0.10), w, h))


def main():
    if len(sys.argv) < 4:
        print(__doc__.strip())
        return 1

    a, b = load(sys.argv[1]), load(sys.argv[2])
    out = sys.argv[3]
    thresh = float(sys.argv[4]) if len(sys.argv) > 4 else 8.0

    cw, ch = min(a.width, b.width), min(a.height, b.height)
    # Bottom-anchored: measured that the extra rows are a title bar appearing at the top.
    a = a.crop((0, a.height - ch, cw, a.height))
    b = b.crop((0, b.height - ch, cw, b.height))

    d = np.abs(np.asarray(a, dtype=np.int16) - np.asarray(b, dtype=np.int16)).mean(axis=2)
    mask = (d > thresh)

    Image.fromarray((mask * 255).astype(np.uint8), mode="L").save(out)

    # Row/column profile, so the region can be described numerically as well as seen.
    rows, cols = mask.mean(axis=1), mask.mean(axis=0)
    def band(profile, n, label):
        step = len(profile) // n
        parts = ["%.0f%%" % (100 * profile[i * step:(i + 1) * step].mean()) for i in range(n)]
        print("  %s: %s" % (label, " | ".join(parts)))

    print("changed=%.2f%% of the cropped frame  (%dx%d, threshold %.0f)"
          % (100 * mask.mean(), cw, ch, thresh))
    band(rows, 4, "top->bottom quarters")
    band(cols, 4, "left->right quarters")
    print("saved %s" % out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
