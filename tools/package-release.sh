#!/usr/bin/env bash
# Assemble a distributable PCSX2-RTX build from bin/ using an explicit allowlist.
# Nothing is copied that is not named here -- bin/ holds the PS2 BIOS, memory cards,
# save states, crash dumps and the Remix runtime, none of which may ship.
set -euo pipefail

BIN="${1:?usage: package-release.sh <bin dir> <staging dir>}"
OUT="${2:?usage: package-release.sh <bin dir> <staging dir>}"

rm -rf "$OUT"
mkdir -p "$OUT"

# --- executable and its runtime DLLs -------------------------------------
# updater.exe is deliberately absent: it updates to upstream PCSX2 releases and
# would overwrite this build with one that has no Remix backend in it.
FILES=(
  pcsx2-qtx64.exe
  Qt6Concurrent.dll Qt6Core.dll Qt6Gui.dll Qt6Svg.dll Qt6Widgets.dll
  SDL3.dll
  freetype.dll harfbuzz.dll jpeg62.dll kddockwidgets-qt6.dll libpng16.dll
  libsharpyuv.dll libwebp.dll libwebpdemux.dll libwebpmux.dll lz4.dll
  plutosvg.dll plutovg.dll ryml.dll shaderc_shared.dll z.dll zstd.dll
  qt.conf
)
for f in "${FILES[@]}"; do
  cp -- "$BIN/$f" "$OUT/$f"
done

# portable.ini keeps settings, memory cards and save states inside this folder
# instead of Documents\PCSX2, so installing this next to a normal PCSX2 cannot
# overwrite that install's configuration.
: > "$OUT/portable.ini"

# --- Qt plugins, D3D12 agility SDK, resources, docs, translations --------
cp -r -- "$BIN/QtPlugins"    "$OUT/QtPlugins"
cp -r -- "$BIN/D3D12"        "$OUT/D3D12"
cp -r -- "$BIN/resources"    "$OUT/resources"
cp -r -- "$BIN/docs"         "$OUT/docs"
cp -r -- "$BIN/translations" "$OUT/translations"

# --- per-game Remix configs (tracked in git, measured settings) ----------
for c in "$BIN"/[A-Z][A-Z][A-Z][A-Z]-[0-9][0-9][0-9][0-9][0-9].conf; do
  [ -e "$c" ] && cp -- "$c" "$OUT/$(basename "$c")"
done

# --- a minimal rtx.conf ---------------------------------------------------
# Only the setting every user needs. The runtime's new GUI input method creates
# a top-level window that swallows keyboard input before the emulator sees it,
# which reads as "keyboard is broken" rather than as a Remix setting.
# The developer install's rtx.conf is NOT shipped: it carries per-title texture
# hash lists that are meaningless anywhere else.
printf 'rtx.useNewGuiInputMethod = False\n' > "$OUT/rtx.conf"

echo "staged: $(find "$OUT" -type f | wc -l) files, $(du -sh "$OUT" | cut -f1)"
