#!/usr/bin/env python3
"""Camera pose from a math3d row-vector world_view matrix (p_view = p_world * M).

usage: cam_from_matrix.py m00 m01 ... m33   (16 floats, row-major, as printed by RMXT)
   or: cam_from_matrix.py --log <emulog>     (parses the 'RMXT world_view' block)

Prints eye position, forward (view -Z), right (view +X), up (view +Y) in WORLD space, so the
fork's 'CAMTRACK fwd/right/pos' line can be compared directly."""
import sys, re
import numpy as np

def pose(M):
    M = np.asarray(M, dtype=float).reshape(4, 4)
    R = M[:3, :3]            # world->view rotation (row-vector)
    t = M[3, :3]             # translation row: p_view = p_world*R + t
    eye = -t @ np.linalg.inv(R)
    # a world direction d maps to view d*R; view axis e_i in world = e_i * R^-1 = column i of R (R orthonormal)
    right = R[:, 0]; up = R[:, 1]; back = R[:, 2]
    return eye, -back, right, up

def main():
    a = sys.argv[1:]
    if len(a) >= 2 and a[0] == "--log":
        txt = open(a[1], encoding="utf-8", errors="replace").read()
        m = re.search(r"RMXT world_view\s*\n((?:.*RMXT\s+[-\d. ]+\n){4})", txt)
        if not m: print("no 'RMXT world_view' block in log"); return 1
        vals = [float(x) for x in re.findall(r"[-+]?\d+\.\d+", m.group(1))]
    else:
        vals = [float(x) for x in a]
    if len(vals) != 16: print("need 16 floats"); return 2
    eye, fwd, right, up = pose(vals)
    f = lambda v: " ".join(f"{x:+.4f}" for x in v)
    print(f"eye     {f(eye)}")
    print(f"forward {f(fwd)}   (view -Z)")
    print(f"right   {f(right)}   (view +X)")
    print(f"up      {f(up)}   (view +Y)")
    return 0

if __name__ == "__main__":
    sys.exit(main())
