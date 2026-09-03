# remix-homebrew — a PS2 test scene with published ground truth

Two small PS2 homebrew programs that render a known scene and write everything they used
(camera, matrices, lights, geometry) into a magic-tagged block in EE RAM. Boot one in the
remix-backend fork and the backend's recovered camera, un-projected geometry and lighting fits
can be checked against exact numbers instead of against a commercial game.

Built and validated 2026-09-03. The VU1 variant's camera is recovered by the fork's ucode
back-slice **exactly**: eye (0.000, 4.000, 18.000), fovY 38.6°, near 1, forward
(0, −0.1790, −0.9838) — all matching the published values to the printed precision, and the
dolly and yaw camera modes track.

## Layout

| path | what |
|---|---|
| `vu1/` | **The real test.** Geometry is transformed on VU1 by ps2sdk's `draw_3D.vsm` microprogram with the world→clip matrix at VU1 data qword 0 — the shape a retail title has and the shape the fork scans/back-slices. Everything is textured (a white texture on the cubes) because the fork drops untextured draws. |
| `ee/` | Control variant on gsKit: transforms on the EE, no VU1 matrix, untextured cubes and a fixed-UV ground. The fork sees its draws but skips them all (`untex`/`fst`) and finds no camera (`cam world 0`). Useful as the "no VU1 matrix" baseline. |
| `tools/run_elf.sh` | Boot an ELF in the fork, wait, archive the log, print the interesting lines, close the emulator. `EMU=<dir>` overrides the emulator. |
| `tools/rmxt_truth.py` | Find and print the RMXT block in an `eeMemory.bin` or a `.p2s` savestate. |
| `tools/cam_from_matrix.py` | Eye/forward/right/up from a math3d row-vector `world_view`, for comparing with the fork's `CAMTRACK` line. |

## Scene

World space throughout, one `world_screen = world_view × view_screen` per frame (math3d,
row-vector: p' = p·M). Camera at (0, 4, 18) pitched −0.18 rad. Frustum ±0.5 × ±0.35 at
near 1, far 200 (fovY 38.58°, 640×448 square pixels). Three cubes: RGB-faced at the origin
(half 1), a **white probe** at (6, 0, −4) whose face colours are the light response, a warm one
at (−6, 1, 3) (half 1.5). Unlit 24×24 ground at y = −1 with a 128×128 RGB24 checker (16 px
cells, red band at t = 0, blue band at s = 0), tessellated 8×8 because the VU1 microprogram
drops any triangle with a vertex outside the frustum.

Lights (ps2sdk math3d convention, intensity = −dot(n, dir), clamped): ambient 0.15;
sun dir (0.4, −0.8, −0.45) normalised, colour (1.0, 0.95, 0.85); fill dir (−0.7, −0.3, 0.6)
normalised, colour (0.25, 0.3, 0.4). Cube faces are flat-lit (exact for cubes), stored as
GS 0x80 == 1.0. `make EXTRA_CFLAGS=-DSUN_ONLY EE_BIN=remixtest_vu1sun.elf` zeroes the fill
for single-light calibration fits.

Camera modes cycle every 600 frames: 0 static, 1 dolly z 18→30→18, 2 yaw sweep ±0.15 rad.

## The RMXT block (0x290 bytes, 16-aligned, refreshed every frame)

```
0x000 magic "RMXT", version, size, frame        0x010 mode, width, height, light_count
0x020 cam_pos   0x030 cam_rot (radians, as fed to create_world_view)
0x040 world_view   0x080 view_screen   0x0C0 world_screen (== VU1 qw 0..3 in vu1/)
0x100 light_dir[4]   0x140 light_col[4]   0x180 light_type[4] (0 ambient, 1 directional)
0x190 cube_center[3]   0x1C0 cube_half   0x1D0 probe cube corners[8]
0x250 frustum l,r,b,t   0x260 near,far,aspect,ground_y   0x270 ground half, cells, tex w, tex h
0x280 magic "TXMR", variant (1 EE, 2 VU1), vu1_matrix_qw, vu1_dbuf (base | offset<<16)
```
The program prints the block's address and the matrices at boot (`RMXT ...` lines; needs
`[Logging] EnableEEConsole = true` in PCSX2.ini) and a camera line every 300 frames.

## Building (ps2dev v2.0.0 under MSYS2)

Toolchain at `C:\msys64\usr\local\ps2dev` (ee-gcc 15.2.0, ps2sdk, gsKit, dvp-as). From Git Bash:

```bash
MSYSTEM=MSYS C:/msys64/usr/bin/bash.exe -lc "export PS2DEV=/usr/local/ps2dev; cd '<this dir>/vu1' && bash ./build.sh"
```

Traps, all hit while bringing this up:
- The Windows binaries are **32-bit MinGW**; they need the i686 runtime DLLs
  (`pacman -S mingw-w64-i686-{gcc-libs,libwinpthread-git,libiconv,gmp,mpfr,mpc,isl,zstd}`) and
  `/mingw32/bin` on PATH. `bash: exit 127` with no message means a missing DLL.
- **Boot from a short, space-free path** (`E:\ps2test\vu1.elf`). PCSX2 copies `host:<path>`
  over EELOAD's `rom0:OSDSYS` slot; a long path overruns into the next argument and the IOP
  prints `loadelf: ... secname <tail of filename>` and loads nothing (`TLB Miss pc=0`).
- `printf` goes through the IOP fileio RPC: call `SifInitRpc(0)` first.
- Never `draw_wait_finish()` after the VU1 batches — nothing in them raises GS FINISH and the
  EE spins forever; only the clear packet ends with `draw_finish`.
- Strip the ELF after link (every ps2sdk sample does).

## Running

```bash
bash tools/run_elf.sh 'E:\ps2test\vu1.elf' 50 vu1
```
Look for `Remix: world camera resolved -- source ucode back-slice, hypothesis ndc/row-major ...
eye (...)` and the `CAMTRACK fwd/right/pos` lines, and compare with the RMXT lines.

## What the fork reported on the VU1 variant (2026-09-03)

- `world camera resolved -- source ucode back-slice, hypothesis ndc/row-major, score 16.00,
  fovY 38.6 deg, near 1, eye (0.000, 4.000, 18.000)`; `cam world 2694 fallback 6` over 2700
  frames (the 6 are boot).
- `CAMTRACK fwd +0.0000 -0.1790 -0.9838 | pos 0.0 4.0 18.0` static; pos z follows the dolly;
  fwd.x sweeps ±0.14 in yaw mode.
- Every draw submitted (`seen 5390 submitted 5390`), two materials (white, checker), all
  draw w in [10, 100).
- MOONFIT (baked-light fit), sun-only build (`vu1sun.elf`), six 900-frame windows: light FROM
  elevation −66 … −70°, azimuth 346 … 355°, contrast 90–98/255, anchor "TOO FEW SAMPLES"
  (up-facing 0–264 verts vs down-facing ~120–140 k). Truth sun FROM elevation +53.0°, azimuth
  318.4° (L toward light = (−0.400, +0.799, +0.449)); fitted L ≈ (−0.058, −0.919, +0.390).
  x and z keep truth's signs (cube normals are oriented correctly); y is inverted and dominant
  because the fork classifies the unlit ground — 73 % of all vertices, constant lum 128 — as
  DOWN-facing even with right-hand-rule +Y winding, and that plane drags the fit under the
  horizon. So this is a normal-orientation / weighting question in the fork, not a fit bug:
  MOONFIT weights by vertex count, and a big constant-colour plane with a mis-signed normal
  owns the answer. The earlier two-light, unfixed-winding run gave −66° / 342°.
- **MESHTRACK (needs `PCSX2_REMIX_STABLEID=1` in the environment, and a mesh ≥ 64 verts more
  than 250 units from the eye — hence the landmark).** Retail-style build: `MESHTRACK id
  044B8450 96 verts centroid 40.2 -113.4 -277.5 dist 321 n -0.000 +0.352 +0.936 facing +0.992`.
  Truth: centroid (40, 10, −300), normal (0, 0, 1). Reflect the truth across the plane through
  the eye whose normal is the camera's up vector (pitch −0.18 rad) and you get centroid
  (40.0, −113.6, −277.5) and normal (0, 0.352, 0.936) — the fork's numbers to the decimal, with
  0.352 = sin(2·0.18). **The recovered world is the true world reflected through the camera's
  horizontal plane**, i.e. view-space y is negated in the un-projection. `facing` stays positive
  because a reflection through a plane containing the eye preserves which side faces it.
- **Control that settles it (`make EXTRA_CFLAGS='-DSUN_ONLY -DNO_YFLIP'`, projection left
  math3d +y-up, PS2 picture upside-down by design):** the fork elected `ndcY/row-major`
  (score 14.00) on its own and MESHTRACK read `centroid 40.2 10.2 -300.0 n +0.000 -0.000
  +1.000`, the truth to one decimal, stable through all three camera modes; the ground became
  up-facing (79,200 verts) and MOONFIT's elevation came out +50.9° against +53.0° (azimuth 357°
  vs 318° is the constant-colour ground and landmark planes still owning the fit). So the
  mirror is decided by the hypothesis election: for a GS-convention clip matrix (y down, which
  is what retail titles fold in) `ndc` wins by the 0.5 handedness dock and the world comes out
  reflected; the un-mirrored world is the `ndcY` hypothesis the dock penalises. `CAMYFLIP=2`
  removes the dock but ties go to `ndc` by family order, and `CAMYFLIP=3` only mirrors the
  published camera. A one-line preference for `ndcY` on GS-convention matrices is the fix, and
  this scene verifies it in 50 s.
- Why this matters for real titles: for a level camera the reflection flips the sun's
  elevation sign and leaves azimuth alone; MOONFIT's up-vs-down anchor then negates the whole
  vector, yielding the right elevation and an azimuth 180° off — exactly the SOCOM symptom
  (fit 58.9° vs authored 230.9° with elevation matching). The same reflection is the natural
  suspect for "lighting swings with the camera" and the world-light projection residuals.
- **The fix in the fork (`PCSX2_REMIX_CAMYDOWN`, RemixSubmit.cpp, 2026-09-03):** every
  hypothesis whose `scale_y < 0` — the readings under which the guest's clip y points down the
  screen: `gs`, `px`, `ndcY`, `auto`, `r6` — gets +1.0 at both scoring sites (frame election
  and per-draw solver), so it outranks the y-up readings `ndc`/`autoY` that only won by the
  0.5 handedness dock. Default 1; `0` restores the shipped election per game. Verification is
  this scene's retail-style build with `PCSX2_REMIX_STABLEID=1`: pass condition is
  `hypothesis ndcY/row-major` with MESHTRACK `centroid 40 10 -300 n 0 0 1`.
  **Verified 2026-09-03 10:47 on the deployed build:** `PCSX2_REMIX_CAMYDOWN = 1`,
  `world camera resolved -- hypothesis ndcY/row-major, score 16.50, eye (0.000, 4.000, 18.000)`,
  MESHTRACK `centroid 40.2 9.8 -300.0 n +0.000 -0.000 +1.000` through all three camera modes,
  ground up-facing (79,200 verts), MOONFIT elevation +49.7° (truth +53.0°, was −30°).
  Backup of the previous emulator build: `pcsx2-qtx64.pre-camydown-20260903.exe`.
- **Pitch sweep (camera mode 3, ±0.10 rad about −0.18; landmark moved to (40, −10, −300) to stay
  in frustum):** on the fixed build the landmark centroid held at (40.2, −10.2, −300.0), normal
  (0, 0, 1.000), through the entire sweep (forward y −0.087 … −0.273). A correctly recovered
  FOV keeps the world still under pitch. SOCOM's "shadows change when I look up/down" is
  therefore not a general fork error but a consequence of its projection being read with
  fovY 112.6° (Remix dev menu confirms 112.6): a vertical stretch about the eye along the
  current view axis is invisible under yaw and deforms under pitch.
- Why the ground reads as down-facing: `smooth_scratch_normals()` takes the right-hand cross
  product `(v1−v0)×(v2−v0)` of the fork's *recovered* world positions (welded by position,
  area-averaged), and `accumulate_moon_fit()` calls `n_y > 0.5` up and `n_y < −0.5` down. The
  ground is wound to give +Y under that exact rule, so a −Y result means the recovered world
  is **mirrored in one axis** relative to the truth world (a mirror flips every derived
  normal's sign) — the handedness the fork's `WORLDFIX` / `m[0][0] < 0` logic exists to decide.
  Next step: track the probe cube's centroid (truth (6, 0, −4)) with MESHTRACK, or compare the
  RGB cube's ±X face positions, to name the mirrored axis; then MOONFIT on this scene should
  land on +53° / 318° once normals are consistent.
