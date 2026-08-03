"""Two-image mean-|diff| metric for the PCSX2 Remix capture arms.

The plan's Verification step 5 assumed colmetric.py did this; it does not -- colmetric.py is a
single-image metric. The prior session computed mean-|diff| inline (p4d.py) and never saved it,
so steps 3a/3b had no differ at all.

Convention matches colmetric.py: the top 10% of the frame is cropped away, because that band
carries the PCSX2 OSD/status text, which changes every frame and would swamp the geometry signal.

MEASURED, and the reason this script is not a two-liner: capture.ps1 does NOT produce a constant
frame size within one run. Two consecutive standing-still captures from the prior session's
rwLit arm are 2100x1116 and 2100x1199 -- same width, 83 rows apart. A naive comparison raises
SIZE MISMATCH on pairs that are genuinely the same scene, which would have blocked steps 3a/3b.
So when sizes differ this crops to the common size and tries both vertical anchors, reporting the
better alignment. That is an approximation, and it says so in the output rather than hiding it.

Reference points, same crop:
  ~3.9/255   two captures of a STATIC scene, standing still -- the noise floor. At or below this
             means "the image did not change".
  ~120/255   standing vs. after a 90-degree turn (measured on rwLit_1 vs rwLit_3). This is what
             a genuinely different image looks like; do not read a 10/255 as "changed a lot".

Usage:  python imgdiff.py <a.png> <b.png> [label]
"""

import sys
from PIL import Image

try:
    import numpy as np
except ImportError:
    np = None

FLOOR = 4.0  # the 3.9/255 static-scene noise floor, rounded up


def load(path):
    im = Image.open(path).convert("RGB")
    w, h = im.size
    return im.crop((0, int(h * 0.10), w, h)), (w, h)


def metrics(a, b):
    """mean |diff| per pixel (channel-averaged), p99, and % of pixels visibly changed."""
    if np is not None:
        d = np.abs(np.asarray(a, dtype=np.int16) - np.asarray(b, dtype=np.int16))
        per_px = d.mean(axis=2)
        return float(per_px.mean()), float(np.percentile(per_px, 99)), float((per_px > 8).mean() * 100.0)

    vals = [(abs(r1 - r2) + abs(g1 - g2) + abs(b1 - b2)) / 3.0
            for (r1, g1, b1), (r2, g2, b2) in zip(a.getdata(), b.getdata())]
    n = max(len(vals), 1)
    srt = sorted(vals)
    return (sum(vals) / n,
            srt[min(int(n * 0.99), n - 1)],
            100.0 * sum(1 for v in vals if v > 8) / n)


def main():
    if len(sys.argv) < 3:
        print(__doc__.strip())
        return 1

    a, size_a = load(sys.argv[1])
    b, size_b = load(sys.argv[2])
    label = sys.argv[3] if len(sys.argv) > 3 else "diff"

    note = ""
    if a.size != b.size:
        cw, ch = min(a.width, b.width), min(a.height, b.height)
        if cw == 0 or ch == 0:
            print("%s: NO OVERLAP %s vs %s" % (label, size_a, size_b))
            return 2
        # Try both vertical anchors; a window that gained a title bar is shifted, not rescaled.
        top = (a.crop((0, 0, cw, ch)), b.crop((0, 0, cw, ch)))
        bot = (a.crop((0, a.height - ch, cw, a.height)), b.crop((0, b.height - ch, cw, b.height)))
        m_top, m_bot = metrics(*top), metrics(*bot)
        if m_bot[0] < m_top[0]:
            mean, p99, changed = m_bot
            anchor = "bottom"
        else:
            mean, p99, changed = m_top
            anchor = "top"
        note = "  [sizes differ %s vs %s -- cropped to %dx%d, %s-anchored]" % (
            size_a, size_b, cw, ch, anchor)
    else:
        mean, p99, changed = metrics(a, b)

    verdict = "STATIC (at/below the %.1f/255 floor)" % FLOOR if mean <= FLOOR else "CHANGED"
    print("%s: mean|diff|=%.2f/255  p99=%.1f  changed_px=%.2f%%  -> %s%s"
          % (label, mean, p99, changed, verdict, note))
    return 0


if __name__ == "__main__":
    sys.exit(main())
