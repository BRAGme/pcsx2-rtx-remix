#!/usr/bin/env bash
# Build remixtest.elf with the ps2dev toolchain installed under MSYS2 /usr/local/ps2dev.
# Run from an MSYS2 shell, or from Git Bash via:
#   C:/msys64/usr/bin/bash.exe -lc "cd '<this dir>' && ./build.sh"
set -euo pipefail
export PS2DEV=${PS2DEV:-/usr/local/ps2dev}
export PS2SDK=$PS2DEV/ps2sdk
export GSKIT=$PS2DEV/gsKit
# The ps2dev v2.0.0 Windows binaries are 32-bit MinGW builds: they need the i686 runtime DLLs
# (pacman -S mingw-w64-i686-{gcc-libs,libwinpthread-git,libiconv,gmp,mpfr,mpc,isl,zstd}).
export PATH=/mingw32/bin:$PATH:$PS2DEV/bin:$PS2DEV/ee/bin:$PS2DEV/iop/bin:$PS2DEV/dvp/bin:$PS2SDK/bin
cd "$(dirname "$0")"
echo "== toolchain: $(command -v mips64r5900el-ps2-elf-gcc)"
mips64r5900el-ps2-elf-gcc --version | head -1
make clean >/dev/null 2>&1 || true
make "$@"
ls -l remixtest.elf
mips64r5900el-ps2-elf-size remixtest.elf 2>/dev/null || true
