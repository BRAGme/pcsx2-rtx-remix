#!/usr/bin/env bash
# deploy_verify.sh <tag>
# Backs up the deployed fork exe/pdb in the user's own naming convention, deploys the fresh
# build from the repo's bin/, then runs the retail-style harness build with STABLEID=1 and
# prints the election, MESHTRACK and MOONFIT lines.
set -u
TAG="$1"
R="/c/Users/Tristan/Documents/GitHub/pcsx2"
EMU="/e/Emulators/PCSX2 RTX Remix"
S="/c/Users/Tristan/AppData/Local/Temp/claude/C--Users-Tristan-Documents-GitHub/8bfe689d-e2b6-4cad-9095-82fd3ccfc412/scratchpad"
DATE="$(date +%Y%m%d)"

echo "=== new build ==="; ls -l "$R/bin/pcsx2-qtx64.exe" "$R/bin/pcsx2-qtx64.pdb"
taskkill //F //IM pcsx2-qtx64.exe >/dev/null 2>&1; taskkill //F //IM NvRemixBridge.exe >/dev/null 2>&1; sleep 1
cp -n "$EMU/pcsx2-qtx64.exe" "$EMU/pcsx2-qtx64.pre-${TAG}-${DATE}.exe" && echo "backup: pcsx2-qtx64.pre-${TAG}-${DATE}.exe"
cp -n "$EMU/pcsx2-qtx64.pdb" "$EMU/pcsx2-qtx64.pre-${TAG}-${DATE}.pdb" 2>/dev/null
cp "$R/bin/pcsx2-qtx64.exe" "$EMU/pcsx2-qtx64.exe" && cp "$R/bin/pcsx2-qtx64.pdb" "$EMU/pcsx2-qtx64.pdb" && echo "deployed: $(md5sum "$EMU/pcsx2-qtx64.exe" | cut -c1-12)"

export PCSX2_REMIX_STABLEID=1
echo; echo "=== VERIFY: retail-style build (y-down clip) under the new election ==="
bash "$S/run_elf.sh" 'E:\ps2test\vu1sun.elf' 45 "verify_${TAG}" >/dev/null 2>&1
L=$(ls -t "$S"/runs/verify_${TAG}-*.log | head -1)
grep "PCSX2_REMIX_CAMYDOWN" "$L" | cut -c1-140
grep "world camera resolved" "$L" | sed 's/depth scale.*//' | cut -c1-190
grep "Remix: CAMTRACK" "$L" | awk 'NR==3||NR==9||NR==14' | sed 's/.*| pos/pos/' | cut -c1-210
grep "MOONFIT" "$L" | tail -1 | sed 's/(under ~3.*up-facing/up-facing/; s/-- anchor.*//' | cut -c1-230
grep "Remix: frame " "$L" | tail -1 | sed 's/| skip:.*| warn/| ... | warn/' | cut -c1-160
echo "=== VERIFY DONE ($L) ==="
