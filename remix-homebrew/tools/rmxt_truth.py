#!/usr/bin/env python3
"""Find and print the remixtest ground-truth block ("RMXT") in EE memory.

usage: rmxt_truth.py <eeMemory.bin | savestate.p2s> [--brief]

The block is 0x290 bytes, 16-byte aligned, magic "RMXT" at +0 and "TXMR" at +0x280, and the
program refreshes it every frame. All matrices are math3d row-vector (p' = p * M)."""
import struct, sys, zipfile

MAGIC0 = b"RMXT"
MAGIC1 = b"TXMR"
SIZES = {1: 0x290, 2: 0x2A0}   # version 2 adds a landmark row before the tail magic

def load(path):
    if path.lower().endswith(".p2s"):
        with zipfile.ZipFile(path) as z:
            return z.read("eeMemory.bin")
    return open(path, "rb").read()

def f4(b, o): return struct.unpack_from("<4f", b, o)
def m4(b, o): return [struct.unpack_from("<4f", b, o + 16 * r) for r in range(4)]
def u4(b, o): return struct.unpack_from("<4I", b, o)

def parse(b, base):
    t = {}
    t["magic0"], t["version"], t["size"], t["frame"] = u4(b, base + 0x00)
    t["mode"], t["width"], t["height"], t["light_count"] = u4(b, base + 0x10)
    t["cam_pos"] = f4(b, base + 0x20)
    t["cam_rot"] = f4(b, base + 0x30)
    t["world_view"] = m4(b, base + 0x40)
    t["view_screen"] = m4(b, base + 0x80)
    t["world_screen"] = m4(b, base + 0xC0)
    t["light_dir"] = [f4(b, base + 0x100 + 16 * i) for i in range(4)]
    t["light_col"] = [f4(b, base + 0x140 + 16 * i) for i in range(4)]
    t["light_type"] = u4(b, base + 0x180)
    t["cube_center"] = [f4(b, base + 0x190 + 16 * i) for i in range(3)]
    t["cube_half"] = f4(b, base + 0x1C0)
    t["probe_corners"] = [f4(b, base + 0x1D0 + 16 * i) for i in range(8)]
    t["frustum"] = f4(b, base + 0x250)
    t["frustum2"] = f4(b, base + 0x260)
    t["ground"] = f4(b, base + 0x270)
    tail = 0x280
    if t["version"] >= 2:
        t["landmark"] = f4(b, base + 0x280)
        tail = 0x290
    t["magic1"], t["variant"], t["vu1_matrix_qw"], t["vu1_dbuf"] = u4(b, base + tail)
    return t

def fmt_m(m):
    return "\n".join("      [%9.4f %9.4f %9.4f %9.4f]" % r for r in m)

def main():
    if len(sys.argv) < 2:
        print(__doc__); return 2
    b = load(sys.argv[1]); brief = "--brief" in sys.argv
    hits = []
    i = b.find(MAGIC0)
    while i >= 0:
        if i % 16 == 0:
            ver, size = struct.unpack_from("<II", b, i + 4)
            tail = 0x290 if ver >= 2 else 0x280
            if SIZES.get(ver) == size and b[i + tail:i + tail + 4] == MAGIC1:
                hits.append(i)
        i = b.find(MAGIC0, i + 1)
    if not hits:
        print("no RMXT block found"); return 1
    for base in hits:
        t = parse(b, base)
        print(f"RMXT @ EE 0x{base:08X}  version {t['version']} variant {t['variant']} frame {t['frame']} mode {t['mode']}  {t['width']}x{t['height']}")
        print(f"  cam_pos {t['cam_pos'][:3]}  cam_rot(rad) {t['cam_rot'][:3]}")
        print(f"  frustum l/r/b/t {t['frustum']}  near/far/aspect/ground_y {t['frustum2']}")
        print(f"  lights ({t['light_count']}):")
        for k in range(t["light_count"]):
            kind = "ambient" if t["light_type"][k] == 0 else "directional"
            print(f"    {k}: {kind:11s} dir {tuple(round(v, 5) for v in t['light_dir'][k][:3])}  col {tuple(round(v, 3) for v in t['light_col'][k][:3])}")
        print(f"  cubes: centers {[c[:3] for c in t['cube_center']]} half {t['cube_half'][:3]}")
        print(f"  ground: half {t['ground'][0]} cells/uvspan {t['ground'][1]} tex {int(t['ground'][2])}x{int(t['ground'][3])}")
        if "landmark" in t:
            print(f"  landmark: centre {t['landmark'][:3]} half {t['landmark'][3]} (normal +Z, faces the camera)")
        if t["variant"] == 2:
            print(f"  VU1: matrix at qword {t['vu1_matrix_qw']}, double buffer base {t['vu1_dbuf'] & 0xFFFF} offset {t['vu1_dbuf'] >> 16}")
        if not brief:
            print("  world_view:\n" + fmt_m(t["world_view"]))
            print("  view_screen:\n" + fmt_m(t["view_screen"]))
            print("  world_screen (= VU1 qw0-3 in the VU1 variant):\n" + fmt_m(t["world_screen"]))
            print("  probe cube corners: " + ", ".join(str(tuple(round(v, 2) for v in c[:3])) for c in t["probe_corners"]))
    return 0

if __name__ == "__main__":
    sys.exit(main())
