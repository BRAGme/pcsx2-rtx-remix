#!/usr/bin/env bash
# shots_session.sh <elf (Windows path)> <label> [seconds=100] [emu dir]
# Boots the ELF and tries a passive window capture every 8 s for <seconds>. A capture only
# succeeds while the emulator is the FOREGROUND window (the Vulkan surface cannot be read
# through PrintWindow), so the user has to bring it to the front; failed attempts are skipped.
set -u
ELFWIN="$1"; LABEL="$2"; SECS="${3:-100}"; EMU="${4:-/e/Emulators/PCSX2 RTX Remix}"
S="/c/Users/Tristan/AppData/Local/Temp/claude/C--Users-Tristan-Documents-GitHub/8bfe689d-e2b6-4cad-9095-82fd3ccfc412/scratchpad"
OUT="$S/shots"; mkdir -p "$OUT"
PS1SCRIPT="$(cygpath -w "$S/shot_window.ps1")"
EXE="pcsx2-qtx64.exe"; [ -f "$EMU/pcsx2-qt.exe" ] && EXE="pcsx2-qt.exe"
PROC="${EXE%.exe}"
kill_emu() { taskkill //F //IM pcsx2-qtx64.exe >/dev/null 2>&1; taskkill //F //IM pcsx2-qt.exe >/dev/null 2>&1; taskkill //F //IM NvRemixBridge.exe >/dev/null 2>&1; taskkill //F //IM NvRemixBridge32.exe >/dev/null 2>&1; }

kill_emu; sleep 2
cd "$EMU" && (./"$EXE" -fastboot "$ELFWIN" >/dev/null 2>&1 &)
sleep 8
n=0; ok=0; t=8
while [ "$t" -lt "$SECS" ]; do
  n=$((n+1))
  f="$OUT/${LABEL}_$(printf '%02d' "$n")_t${t}s.png"
  if powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$PS1SCRIPT" -Out "$(cygpath -w "$f")" -ProcName "$PROC" >/dev/null 2>&1; then
    ok=$((ok+1)); echo "t=${t}s captured $(basename "$f")"
  else
    echo "t=${t}s skipped (emulator not in front)"
  fi
  sleep 8; t=$((t+8))
done
kill_emu
echo "captured $ok of $n attempts"; ls -l "$OUT"/${LABEL}_*.png 2>/dev/null
