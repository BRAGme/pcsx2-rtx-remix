# RTX Remix backend test harness

Measurement scripts for the `remix-backend` branch. Committed because they previously lived only in
a `%TEMP%` scratchpad, where a cleanup would have destroyed them — several encode measurements and
traps that cost real time to find.

Every script defaults `-Bin` to the repo build at `..\..\bin`, resolved from the script's own
location, so a fresh checkout needs no editing. Captures and A/B results go to `-Scratch`, which
defaults to `%TEMP%\remix-harness` and is created if missing; `abcam.ps1` derives `-Stash`,
`-Results` and `-Fingerprint` from it, and `-Stash\armA` / `-Stash\armB` are an **input** you
populate with the two builds before the run.

Two paths are yours, not the repo's, and no default is baked in for either:

| variable | what it is | used by |
|---|---|---|
| `PCSX2_TEST_ISO_DIR` | folder holding your PS2 test images | all five launching scripts |
| `PCSX2_TEST_INSTALL` | the deployed install `abcam.ps1` swaps builds into | `abcam.ps1` |

Set them once per shell (or pass `-IsoDir` / `-Install`):

```powershell
$env:PCSX2_TEST_ISO_DIR = "D:\PS2 Games"
$env:PCSX2_TEST_INSTALL = "D:\Emulators\PCSX2 RTX Remix"
```

A script that needs one and does not have it stops immediately and names the variable. That check
is deliberate: PCSX2 accepts a bad image path and only reports `Requested filename does not exist`
seconds later, which reads as a crashed run rather than a typo -- and `abcam.ps1` has been bitten
by exactly that before. The `-Iso` parameters still default to the title each script was written
around, so only the folder is yours to supply.

`build.cmd`'s Visual Studio path is still a one-line edit.

## The scripts

| script | what it does |
|---|---|
| `build.cmd` | `vcvars64.bat` + `msbuild PCSX2_qt.sln /m /p:Configuration=Release /p:Platform=x64`. `clang-cl` is absent on this machine so the `CMakePresets.json` presets are unusable. |
| `deploy.ps1` | **The important one.** Boots SOCOM's deploy menu (slot 3), survives the startup window there, then presses Cross to load into the mission from an already-stable session. |
| `ghashstill.ps1` | N stills with **zero input**, so a frame-to-frame diff measures churn rather than a changed view. |
| `rotwalk.ps1` | Motion arm: stills, a 360° turn in three steps, then walk forward/back, capturing at each stage. Installs temporary keyboard pad bindings and restores them. |
| `arm.ps1` | Survival accounting over N launches. For crash arms only. |
| `capture.ps1` | DPI-aware `PrintWindow(PW_RENDERFULLCONTENT)` of the render window. |
| `send.ps1` | Keyboard input via `keybd_event`. Keyboard only — no mouse-look. |
| `colmetric.py` | Single-image metric: `lit_px`, `mean_sat`, `mean_lum`, `coloured_px`. |
| `imgdiff.py` | Two-image mean-\|diff\|. Handles the capture size mismatch. |
| `diffmask.py` | Saves a mask of *where* two captures differ, plus a row/column profile. |

## Why `deploy.ps1` exists

The `0x60D0DEAD` GPU hang lands within a fraction of a second of `renderer is live`, and **surviving
that window buys minutes**. Slot 3 is a light scene (~3 draws/frame) that boots reliably; slots 1
and 2 load the full mission straight into the fragile window and die in ~5 s. Before this route
there was no way to measure or capture SOCOM in-mission at all — every arm died first.

The menu is invisible in Remix mode (D3D11 is surfaceless, sprites are never submitted), so it
navigates blind and detects mission entry from `maxpos` crossing ~1000. Budget ~5 attempts for a
*settled* mission (`seen > 150000`).

### To see a menu at all: boot it on D3D11

Set `Renderer = 3` in `bin\inis\PCSX2.ini` and the screen renders normally. This is how the
navigation was worked out, and it is the way to answer any "what is on screen" question that the
Remix path cannot. It also gives a **reference image of what the game is supposed to look like**,
which is worth having next to a Remix capture.

Established that way: **slot 3 is the KINGFISHER mission briefing** — objectives list, and a gold
three-pointed emblem at the bottom which is what renders as a giant blown-out triangle in Remix
mode. On D3D11 at 59.93 fps, **one Cross press ~20 s in loads the mission**.

### Entry is limited by the GPU hang, not by navigation

Four strategies, and the honest summary is that only the bad one is distinguishable:

| approach | entered |
|---|---|
| 8–10 presses @2.2 s (~25 s total) | ~1 / 4 |
| one press then a 24 s poll, repeated (~75 s) | 3 / 5 |
| 4 presses front-loaded @2.5 s (~32 s) | **0 / 6** |
| periodic presses @5 s across 110 s | 1 / 5 |

Front-loading clearly fails, so a press has to land late; but entry has also been observed at
t+20 s, so there is no fixed threshold to hit. The failures are overwhelmingly `died during nav` —
pressing Cross starts loading heavy mission geometry, and *that* is what triggers the hang. **Do not
spend more time tuning the key sequence; the ceiling is the upstream crash.** Current defaults
(8 s gap across a 100 s window) are a reasonable middle, not an optimum.

## Traps these encode — all of them cost time

- **`capture.ps1 -TitleMatch "."` captures PCSX2 *dialogs*, not the game.** Any title matches, and it
  saves whichever window has the most non-black pixels, so a Settings dialog full of white text beats
  a dim path-traced scene. Seven "motion" captures were the Settings dialog. Always pass a real
  title; a `WRONG_WINDOW` guard now exits 3 rather than returning a plausible wrong image.
- **`PrintWindow` intermittently returns black** on the Remix surface, roughly alternating
  (`lit_px` 2,343,600 / 11,100 / 0). **Gate every image comparison on `lit_px`** or it measures
  "unrendered vs rendered" and reports a meaningless ~125/255.
- **Capture size is not constant within a run** — 2100x1240 vs 2100x1332, the extra rows a title bar
  at the top. `imgdiff.py` crops to the common size and tries both anchors.
- **Never trust bare `Get-Process` for liveness.** Windows leaves zombies with 0 threads and 0 CPU
  that make `[bool](Get-Process ...)` return `True`; this produced two false "it survived" reports.
  Assert `Threads.Count -gt 0` *and* an advancing `Remix: frame` in `emulog.txt`.
- **Exit codes are signed.** `0xFEFEFEFE` arrives as `-16843010` and throws on `[uint32]`, which
  suppresses the `DIED` line and makes a crashed run look like a survival.
- **`bin\sstates` and the deployed `sstates` are not in sync.** A missing state gives
  `ReportErrorAsync: Startup Error` — an audible Windows error beep and a **clean exit 0 in ~1.5 s**.
  That is a missing save state, not the GPU hang; `0x60D0DEAD` at ~5 s is the hang.
- **A build can silently fail to link** with `LNK1104: cannot open file pcsx2-qtx64.exe` when a
  running PCSX2 holds the exe, and the next arm then measures the *old* binary. **Check the exe
  timestamp moved before trusting a result.**
- **Run-to-run variance on identical code measured `mean_lum` 59.9 -> 98.0.** Larger than most
  changes produce. Use a binary instrument (the albedo debug view reads 0 or ~580,000) or many runs
  — never a single normal-view capture.
- **Measuring from a menu is worthless**, and the slot labels lied about which state was which:
  **slot 2 and slot 1 are in-mission, slot 3 is the deploy menu.**

## Useful debug views

`DXVK_RTX_DEBUG_VIEW_INDEX=<n>`, verified working for API-submitted geometry:

- `23` — albedo. Validated against Rainbow Six 3 (`lit_px` 2,421,139, `mean_lum` 111) before being
  trusted on a null reading.
- `277` — geometry hash. Flat colour per object; colours reshuffling between frames means churn.
