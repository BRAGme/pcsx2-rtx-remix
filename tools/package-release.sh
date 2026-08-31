#!/usr/bin/env bash
# Assemble a distributable PCSX2-RTX build from bin/ using an explicit allowlist.
# Nothing is copied that is not named here -- bin/ holds the PS2 BIOS, memory cards,
# save states, crash dumps and the Remix runtime, none of which may ship.
set -euo pipefail

BIN="${1:?usage: package-release.sh <bin dir> <staging dir>}"
OUT="${2:?usage: package-release.sh <bin dir> <staging dir>}"
# Repo root, resolved from this script rather than the caller's cwd.
REPO="$(cd "$(dirname "$0")/.." && pwd)"

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

# The allowlist above deliberately ships pcsx2-qtx64.exe without its .pdb -- no debug
# symbols in a user build. The recursive copies bypass that intent: bin\QtPlugins now
# carries a .pdb beside every plugin (it did not when preview 1 was cut, so the zip
# silently grew 17 symbol files). Strip them back out rather than letting the directory
# copies decide what ships.
find "$OUT" -type f -name '*.pdb' -delete

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

# --- SETUP.txt ------------------------------------------------------------
# The zip's own instructions. This was hand-written and hand-added for preview 1,
# which meant the packaging step could not reproduce its own output -- a second
# release cut with this script alone would have silently shipped without it, while
# the README went on telling users to read it. Templated now: prose in
# tools/SETUP.txt.in, and only the volatile build-provenance line is
# substituted here, so the file cannot drift from the build it ships beside.
GIT_SHORT="$(git -C "$REPO" rev-parse --short=9 HEAD)"
GIT_REV="$(git -C "$REPO" describe --tags 2>/dev/null || echo "$GIT_SHORT")"
sed -e "s/@GIT_SHORT@/$GIT_SHORT/g" -e "s/@GIT_REV@/$GIT_REV/g" \
    "$REPO/tools/SETUP.txt.in" > "$OUT/SETUP.txt"

echo "staged: $(find "$OUT" -type f | wc -l) files, $(du -sh "$OUT" | cut -f1)"

# --- the distributable zip ------------------------------------------------
# Everything lives under a single top-level pcsx2-rtx-remix/ folder, so extracting
# the zip cannot scatter 260 files across whatever directory the user was in. This
# wrapping was also done by hand for preview 1 and is now part of the script, for
# the same reason as SETUP.txt.
#
# Zipping is skipped when the `zip` binary is absent rather than failing the run --
# the staged tree is still correct and can be archived by any other means.
ZIP_NAME="pcsx2-rtx-remix-$GIT_SHORT-win64.zip"
ZIP_PATH="$(cd "$(dirname "$OUT")" && pwd)/$ZIP_NAME"
# Git Bash on Windows ships no `zip`, but 7-Zip is a build-dependency prerequisite
# anyway, so it is the fallback rather than a hard failure.
SEVENZIP=""
if command -v 7z >/dev/null 2>&1; then
  SEVENZIP="7z"
elif [ -x "/c/Program Files/7-Zip/7z.exe" ]; then
  SEVENZIP="/c/Program Files/7-Zip/7z.exe"
fi

if command -v zip >/dev/null 2>&1 || [ -n "$SEVENZIP" ]; then
  WRAP="$(dirname "$OUT")/.pkgwrap"
  rm -rf "$WRAP"
  mkdir -p "$WRAP"
  cp -r -- "$OUT" "$WRAP/pcsx2-rtx-remix"
  rm -f "$ZIP_PATH"
  if command -v zip >/dev/null 2>&1; then
    ( cd "$WRAP" && zip -qr "$ZIP_PATH" pcsx2-rtx-remix )
  else
    ( cd "$WRAP" && "$SEVENZIP" a -tzip -bso0 -bsp0 "$ZIP_PATH" pcsx2-rtx-remix )
  fi
  rm -rf "$WRAP"
  echo "zipped: $ZIP_PATH ($(du -h "$ZIP_PATH" | cut -f1))"
else
  echo "zip: SKIPPED -- neither 'zip' nor 7-Zip found. Staged tree is at $OUT;"
  echo "     archive it yourself with a single top-level pcsx2-rtx-remix/ folder."
fi
