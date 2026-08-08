# Parses the A/B logs abcam.ps1 collects and prints the camera clip-split verdict.
#
#   python abcam_report.py <results-dir>
#
# Counters are session-cumulative -- s_stats is only cleared at shutdown (RemixSubmit.cpp:5300) --
# so the LAST occurrence of each block in a log is that launch's total, not a delta to sum.
#
# Matching is on substrings, never on line anchors: [Logging] EnableTimestamps = true prefixes
# every emulog line with "[  348.4722] ".
#
# Line formats (RemixSubmit.cpp:3376-3404, :3469-3478, :3497-3498).

import os
import re
import statistics
import sys

RE_FRAME = re.compile(r"Remix: frame (\d+) \| seen (\d+) submitted (\d+)")
RE_CAM = re.compile(r"cam world (\d+) fallback (\d+)")
RE_SKY = re.compile(r"\| sky (\d+) cutout (\d+)")
RE_CAND = re.compile(r"\| cand (\d+) \(now (\d+)\)")
RE_ANCHOR = re.compile(
    r"split-reject (\d+) score-reject (\d+) degenerate-reject (\d+) "
    r"scale-reject (\d+) extent-reject (\d+) accept (\d+)")
RE_STAGES = re.compile(r"Remix: split refusals by stage -- (.+?)\s*$")

# The floor, pre-declared in the plan before any data existed.
FLOOR_LAUNCHES = 8
FLOOR_CAND = 100000


def parse_log(path):
    """Last full counter block in one launch's emulog, plus the marker checks."""
    r = {
        "path": path, "name": os.path.basename(path),
        "live": False, "fill": False, "createlight_failed": False,
        "frames": 0, "seen": 0, "submitted": 0,
        "world": 0, "fallback": 0, "sky": 0,
        "cand": 0, "split_reject": 0, "score_reject": 0, "degenerate_reject": 0,
        "scale_reject": 0, "extent_reject": 0, "accept": 0,
        "stages": {},
    }
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            if "renderer is live" in line:
                r["live"] = True
            if "scene lighting -- distance-independent fill" in line:
                r["fill"] = True
            if "CreateLight failed" in line:
                r["createlight_failed"] = True

            if "Remix: frame " in line:
                m = RE_FRAME.search(line)
                if m:
                    r["frames"], r["seen"], r["submitted"] = (int(x) for x in m.groups())
                m = RE_CAM.search(line)
                if m:
                    r["world"], r["fallback"] = int(m.group(1)), int(m.group(2))
                m = RE_SKY.search(line)
                if m:
                    r["sky"] = int(m.group(1))

            if "vu kicks" in line:
                m = RE_CAND.search(line)
                if m:
                    r["cand"] = int(m.group(1))
                m = RE_ANCHOR.search(line)
                if m:
                    (r["split_reject"], r["score_reject"], r["degenerate_reject"],
                     r["scale_reject"], r["extent_reject"], r["accept"]) = (int(x) for x in m.groups())

            if "split refusals by stage" in line:
                m = RE_STAGES.search(line)
                if m:
                    stages = {}
                    for part in m.group(1).split("|"):
                        bits = part.split()
                        if len(bits) == 2 and bits[1].isdigit():
                            stages[bits[0]] = int(bits[1])
                    if stages:
                        r["stages"] = stages

    # The six pre-declared positive checks. A launch failing any one is NO-DATA and is listed
    # but never pooled -- a half-booted launch would otherwise dilute whichever arm drew it.
    checks = [
        ("renderer-live", r["live"]),
        ("fill-light", r["fill"]),
        ("no-createlight-fail", not r["createlight_failed"]),
        ("submitted>0", r["submitted"] > 0),
        ("sky>0", r["sky"] > 0),
        ("cand>0", r["cand"] > 0),
    ]
    r["checks"] = checks
    r["failed"] = [n for n, ok in checks if not ok]
    r["data"] = not r["failed"]
    return r


def pool(rows):
    keys = ("frames", "seen", "submitted", "world", "fallback", "sky", "cand",
            "split_reject", "score_reject", "degenerate_reject", "scale_reject",
            "extent_reject", "accept")
    t = {k: sum(r[k] for r in rows) for k in keys}
    t["singular"] = sum(r["stages"].get("singular", 0) for r in rows)
    t["stages"] = {}
    for r in rows:
        for k, v in r["stages"].items():
            t["stages"][k] = t["stages"].get(k, 0) + v
    t["n"] = len(rows)
    return t


def med(rows, key):
    return statistics.median([r[key] for r in rows]) if rows else 0


def world_share(t):
    d = t["world"] + t["fallback"]
    return (t["world"] / d) if d else 0.0


def sing_over_cand(t):
    return (t["singular"] / t["cand"]) if t["cand"] else 0.0


def accept_per_kframe(t):
    return (1000.0 * t["accept"] / t["frames"]) if t["frames"] else 0.0


def arm_table(arm, rows, nodata):
    print("\n== ARM %s ==" % arm)
    print("  %-16s %8s %8s %10s %10s %8s %8s %8s %8s"
          % ("launch", "frames", "sky", "cand", "singular", "split-r", "accept", "world", "fallb"))
    for r in rows:
        print("  %-16s %8d %8d %10d %10d %8d %8d %8d %8d"
              % (r["name"], r["frames"], r["sky"], r["cand"], r["stages"].get("singular", 0),
                 r["split_reject"], r["accept"], r["world"], r["fallback"]))
    for r in nodata:
        print("  %-16s NO-DATA (failed: %s)" % (r["name"], ", ".join(r["failed"])))

    if not rows:
        print("  no DATA launches")
        return None

    t = pool(rows)
    print("  ---")
    print("  DATA launches            : %d   (NO-DATA %d)" % (t["n"], len(nodata)))
    print("  pooled frames            : %d      (per-launch median %g)" % (t["frames"], med(rows, "frames")))
    print("  pooled submitted         : %d" % t["submitted"])
    print("  pooled cand      (C)     : %d      (per-launch median %g)" % (t["cand"], med(rows, "cand")))
    print("  pooled singular  (S)     : %d" % t["singular"])
    print("  pooled split-reject (R)  : %d" % t["split_reject"])
    print("  pooled accept    (A)     : %d      (per-launch median %g)" % (t["accept"], med(rows, "accept")))
    print("  pooled cam world (W)     : %d      (per-launch median %g)" % (t["world"], med(rows, "world")))
    print("  pooled cam fallback (FB) : %d      (per-launch median %g)" % (t["fallback"], med(rows, "fallback")))
    print("  ---")
    print("  singular/cand   = %d/%d = %.4f" % (t["singular"], t["cand"], sing_over_cand(t)))
    print("  singular/split-reject = %d/%d = %.4f"
          % (t["singular"], t["split_reject"], (t["singular"] / t["split_reject"]) if t["split_reject"] else 0.0))
    print("  W/(W+FB)        = %d/%d = %.4f" % (t["world"], t["world"] + t["fallback"], world_share(t)))
    print("  accept per 1000 frames = 1000*%d/%d = %.4f" % (t["accept"], t["frames"], accept_per_kframe(t)))
    print("  post-split gates: score-reject %d degenerate-reject %d scale-reject %d extent-reject %d"
          % (t["score_reject"], t["degenerate_reject"], t["scale_reject"], t["extent_reject"]))
    order = sorted(t["stages"].items(), key=lambda kv: -kv[1])
    print("  stage flow: " + " | ".join("%s %d" % (k, v) for k, v in order))
    return t


def verdict(ta, tb, na, nb):
    """The rules, exactly as pre-declared in the plan before the run."""
    lines = []
    floor_ok = (na >= FLOOR_LAUNCHES and nb >= FLOOR_LAUNCHES
                and ta and tb and ta["cand"] >= FLOOR_CAND and tb["cand"] >= FLOOR_CAND)
    if not floor_ok:
        lines.append("CONFOUNDED -- floor not met (need >= %d DATA launches AND >= %d pooled cand per arm;"
                     % (FLOOR_LAUNCHES, FLOOR_CAND))
        lines.append("   got A: %d launches / %s cand, B: %d launches / %s cand)."
                     % (na, ta["cand"] if ta else 0, nb, tb["cand"] if tb else 0))
        lines.append("   No solver verdict. The survival split is itself the finding.")
        return "CONFOUNDED", lines

    wa, wb = world_share(ta), world_share(tb)
    sa, sb = sing_over_cand(ta), sing_over_cand(tb)
    aa, ab = accept_per_kframe(ta), accept_per_kframe(tb)
    dw = wb - wa

    lines.append("A world-share %.4f | B world-share %.4f | delta %+.4f" % (wa, wb, dw))
    lines.append("A singular/cand %.4f | B singular/cand %.4f | B/A %s"
                 % (sa, sb, ("%.4f" % (sb / sa)) if sa else "n/a"))
    lines.append("A accept/1000f %.4f | B accept/1000f %.4f" % (aa, ab))

    # NO HEADROOM is checked before the others: if A already anchors, the experiment cannot
    # measure an improvement in the thing it was built to measure.
    if wa >= 0.9:
        lines.append("NO HEADROOM -- arm A already reaches W/(W+FB) >= 0.90, so the camera solve was")
        lines.append("   not the blocker in this lighting/sky configuration. Patch verdict limited to")
        lines.append("   accept-rate and stage-flow, both reported above.")
        return "NO HEADROOM", lines

    if dw >= 0.30:
        lines.append("PATCH WORKS -- B's world share exceeds A's by >= 0.30 absolute.")
        return "PATCH WORKS", lines
    if ta["accept"] == 0 and tb["accept"] >= 100 and wb >= 0.5:
        lines.append("PATCH WORKS -- A pooled 0 accepts; B pooled %d accepts at world share %.4f."
                     % (tb["accept"], wb))
        return "PATCH WORKS", lines

    sing_dropped_2x = (sa > 0 and sb <= sa / 2.0)
    no_improve = (dw < 0.05 and ab <= max(aa, 0.0))
    if sing_dropped_2x and dw < 0.05:
        lines.append("DIAGNOSIS WRONG / INCOMPLETE -- singular/cand fell %.2fx in B but world share moved"
                     % (sa / sb if sb else float("inf")))
        lines.append("   only %+.4f (< 0.05). The candidates now die later; new binding stage below." % dw)
        order = sorted(tb["stages"].items(), key=lambda kv: -kv[1])
        if order:
            lines.append("   B's binding stage is now '%s' (%d), then %s."
                         % (order[0][0], order[0][1],
                            ", ".join("%s %d" % (k, v) for k, v in order[1:4])))
        lines.append("   Post-split gates in B: score-reject %d degenerate-reject %d scale-reject %d extent-reject %d"
                     % (tb["score_reject"], tb["degenerate_reject"], tb["scale_reject"], tb["extent_reject"]))
        return "DIAGNOSIS WRONG / INCOMPLETE", lines

    ratio_flat = (sa > 0 and 0.8 <= (sb / sa) <= 1.25)
    if aa == 0 and ab == 0:
        accept_flat = True
    elif aa > 0:
        accept_flat = 0.5 <= (ab / aa) <= 2.0
    else:
        accept_flat = False
    if ratio_flat and accept_flat and abs(dw) < 0.05:
        lines.append("NO EFFECT -- singular/cand ratio B/A %.4f in [0.8, 1.25], accept rate within"
                     % (sb / sa))
        lines.append("   [0.5x, 2x], world-share delta %+.4f (< 0.05)." % dw)
        lines.append("   Re-verify the exe hash chain in runlog.csv; if intact, the invert stage was")
        lines.append("   not the binding constraint in this configuration.")
        return "NO EFFECT", lines

    lines.append("NO PRE-DECLARED RULE TRIGGERED -- reporting the numbers without a verdict label.")
    return "NO RULE", lines


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "."
    arms = {}
    for arm in ("A", "B"):
        d = os.path.join(root, "arm" + arm)
        logs = sorted(f for f in os.listdir(d)
                      if f.startswith("launch-") and f.endswith(".log")
                      and not f.endswith(".dxvk.log")) if os.path.isdir(d) else []
        arms[arm] = [parse_log(os.path.join(d, f)) for f in logs]

    print("A/B camera clip-split report -- %s" % os.path.abspath(root))
    print("arm A = remix-backend @ a9c9d68b4 (full 4x4 inverse)")
    print("arm B = remix-camera-clipsplit @ 7bee9336c (columns {0,1,3})")

    totals, counts = {}, {}
    for arm in ("A", "B"):
        rows = [r for r in arms[arm] if r["data"]]
        nodata = [r for r in arms[arm] if not r["data"]]
        totals[arm] = arm_table(arm, rows, nodata)
        counts[arm] = len(rows)

    print("\n== VERDICT ==")
    label, lines = verdict(totals["A"], totals["B"], counts["A"], counts["B"])
    for ln in lines:
        print("  " + ln)
    print("\n  VERDICT: %s" % label)
    return 0


if __name__ == "__main__":
    sys.exit(main())
