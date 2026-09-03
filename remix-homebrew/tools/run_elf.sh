#!/usr/bin/env bash
# run_elf.sh <elf (Windows path)> [seconds=40] [label]
# Boots an ELF in the PCSX2 remix-backend fork, waits, archives the log, prints the interesting
# lines, and closes the emulator (standing OK to kill it and the bridge helpers).
set -u
ELFWIN="$1"; SECS="${2:-40}"; LABEL="${3:-run}"
# EMU can be overridden (e.g. stock PCSX2) to separate fork behaviour from emulator behaviour.
EMU="${EMU:-/e/Emulators/PCSX2 RTX Remix}"
LOG="$EMU/logs/emulog.txt"
OUT="/c/Users/Tristan/AppData/Local/Temp/claude/C--Users-Tristan-Documents-GitHub/8bfe689d-e2b6-4cad-9095-82fd3ccfc412/scratchpad/runs"
mkdir -p "$OUT"
STAMP="$(date +%H%M%S)"
ARCHIVE="$OUT/${LABEL}-${STAMP}.log"

kill_emu() { taskkill //F //IM pcsx2-qtx64.exe >/dev/null 2>&1; taskkill //F //IM NvRemixBridge.exe >/dev/null 2>&1; taskkill //F //IM NvRemixBridge32.exe >/dev/null 2>&1; }

kill_emu; sleep 2
cd "$EMU" && (./pcsx2-qtx64.exe -fastboot "$ELFWIN" >/dev/null 2>&1 &)
sleep "$SECS"
alive="$(tasklist 2>/dev/null | grep -ci pcsx2-qtx64 || true)"
cp "$LOG" "$ARCHIVE" 2>/dev/null
kill_emu

echo "=== $LABEL: elf=$ELFWIN secs=$SECS alive_at_end=$alive archive=$ARCHIVE ==="
echo "--- boot ---"; grep -n "ELF Loading\|Remix: runtime initialized\|Remix: renderer is live" "$ARCHIVE" | cut -c1-160
echo "--- EE/IOP console + RMXT (first 60 lines after ELF load, stats lines filtered) ---"
awk '/ELF Loading/{f=1;next} f' "$ARCHIVE" | grep -v "Remix: \(frame\|submitted w\|Z->w\|vertex colour\|mat live\|vu kicks\|per-draw\|overlay\|hold-empty\|CAMTRACK\|LIGHTMAPFOLD\)" | head -60 | cut -c1-220
echo "--- RMXT lines (all) ---"; grep "RMXT" "$ARCHIVE" | head -60 | cut -c1-200
echo "--- Remix stats: last frame / vu / camera lines ---"
grep "Remix: frame " "$ARCHIVE" | tail -1 | cut -c1-300
grep "Remix: vu kicks" "$ARCHIVE" | tail -1 | cut -c1-300
grep "Remix: hold-empty\|Remix: CAMTRACK" "$ARCHIVE" | tail -2 | cut -c1-260
grep -i "Remix:.*\(cam world\|camera accepted\|camera installed\|EECAM mode\|split\b\|candidate\)" "$ARCHIVE" | tail -6 | cut -c1-260
echo "--- errors / exceptions ---"; grep -i -n "exception\|tlb miss\|crash\|panic\|EE: \|IOP: " "$ARCHIVE" | grep -v "EECAM read FAILED" | head -10 | cut -c1-200
echo "=== done: $LABEL ==="
