Second binary build of the PCSX2 RTX Remix backend. Three weeks of work since preview 1, and the headline is that a defect everyone saw — flat, untextured-looking surfaces — turned out to be a two-line enum bug rather than anything to do with texture binding.

This is still a **research build**. The "What does not work" list below is not boilerplate, and no title here is playable start to finish. The [RPCS3 backend](https://github.com/BRAGme/rpcs3-rtx/releases) remains further along.

---

## What changed since preview 1

**The flat-albedo defect was an enum mismatch.** D3D9 enum values were being written into fields the runtime casts straight to Vulkan enums. `textureColorArg1Source = 2` meant `D3DTA_TEXTURE` to us and `VertexColor0` to the runtime — so the colour stage was told to take surface colour from vertex colour, with the texture not named as an argument at all. `MODULATE` (4) likewise read as `Modulate2x`, doubling brightness on top of it. That is the reported "textures bind but carry no detail, the colour comes from vertex colour" look, and it was active by default on every title in preview 1.

**The "flickering triangles" were empty present windows, not z-fighting.** A game rendering below the vsync rate submits zero geometry on every Nth presented window, and that window was being presented empty — the world appearing to explode into spikes for one frame in three. Exposure-invariant NCC settled it: clean-to-clean frames scored 0.94–0.99, shattered-to-shattered only 0.12–0.43, so the bad frames did not correlate even with each other, which rules out a tonemapper oscillation. `PCSX2_REMIX_HOLDEMPTY` re-presents the previous window as a true duplicate. Period-3 signature gone, `offcadence 0` over 14,098 holds. Confirmed on Combined Assault and SOCOM 1.

**PS2 text is drawn as sprites**, and the first classification gate returned on any non-triangle primclass about 900 lines before the UI classifier could see it — discarding all of it.

**USD capture was broken for every game by a locale bug.** PCSX2 calls `std::locale::global(std::locale(""))`; the Remix runtime builds prim names with a bare `stringstream`, so on a grouping locale hex hashes came out comma-separated — and commas are not valid in a USD identifier. Every capture failed 18 ms in with `Used null prim`. Captures now work: 3 meshes / 37 materials / 71 textures on a Rainbow Six 3 level.

**Rainbow Six 3's camera is now read out of EE memory** rather than back-sliced from a VU1 microprogram, which sidesteps the candidate-set problem entirely for that title. Verified live — position tracks a multi-thousand-unit walk, yaw follows every turn, Z steps on stairs. Its shadow pass is dropped (characters re-rendered from the light's viewpoint, which un-project into duplicate bodies drifting through walls — 1.3% of draws carrying 24% of the vertices), its baked lightmaps are folded in, and the synthetic projection was calibrated against a physical target for the first time: lamp-to-fixture residual 14.0 units at fov 60, **1.4 at fov 70**.

**Sky is classified by depth, not draw ordinal.** Combined Assault's backdrop is draw 982 of 999, so an ordinal gate could never reach it — and what the ordinal *did* catch was near-field terrain, which is worse than doing nothing.

**Render-to-texture passes are no longer submitted as world geometry.** Un-projecting an off-screen pass with the main camera is meaningless; one such draw covered 62×75 pixels on screen and came out with a world extent of 115,012 units.

---

## You need two things that are not in the zip

### 1. A Remix runtime on the extended API line

**Stock NVIDIA RTX Remix will not work**, for two independent reasons: the version handshake rejects it (the extended line reserves `REMIXAPI_VERSION_MINOR = 1000`, stock is on `0.6.x`), and stock has no `remixapi_CreateTexture` — which this backend needs, because PS2 textures are decoded out of emulated memory and there is no file on disk to point at.

| | |
|---|---|
| Known-good source | [`RemixProjGroup/dxvk-remix`](https://github.com/RemixProjGroup/dxvk-remix), maintainer Kim2091 |
| Release tag | `remix-plus-1.5.1` |
| Asset | `Remix_Plus_v1.5.1_x64_games_release.zip` |

**Which runtime was actually tested.** Everything measured here ran against a *numos3* build of dxvk-remix — it reports `dxvk-remix (remix-main+abbae23d)` — not against the 1.5.1 tag above. Both sit on the same API line (numos3's header is `0.1000.1`, this build's is `0.1000.0`, and the handshake only breaks on the minor), so either should connect. But only the numos3 build has actually run this backend for any length of time. Treat 1.5.1 as expected-to-work rather than verified, and if you see behaviour these notes do not describe, the runtime is the first variable to change.

Unzip it into a **`remix\` subfolder** next to `pcsx2-qtx64.exe`:

```
pcsx2-qtx64.exe
remix\d3d9.dll
remix\...
```

Do **not** drop `d3d9.dll` beside the executable — anything else in the process that resolves that name would pick it up. You can also set `PCSX2_REMIX_DLL`, or point `RuntimePath` under `[Remix]` in the settings.

It isn't bundled because that folder carries third-party redistributables (`nvngx_dlss*.dll`, `cudart64_13.dll`, `GFSDK_Aftermath`, `libxess.dll`, `amd_fidelityfx_vk.dll`) with their own terms, inside someone else's fork build. Get it from the people who made it.

### 2. A PS2 BIOS

Same as stock PCSX2 — dump it from a console you own. No BIOS, games or keys are included.

---

## Turning it on

`Settings → Graphics → Renderer → RTX Remix`

There's a `Settings → RTX Remix` page for the backend's own knobs. Every knob also has a `PCSX2_REMIX_*` environment variable, and a variable that is already set beats both the GUI and the per-game file — deliberately, so an A/B harness stays authoritative. Full table in [`docs/Remix/KNOBS.md`](https://github.com/BRAGme/pcsx2-rtx-remix/blob/remix-backend/docs/Remix/KNOBS.md).

**Six per-game configs now ship**, up from two. Two are curated from measurements taken on those titles; the other four are transplants and say so in their own headers:

| file | title | status |
|---|---|---|
| `SCUS-97545.conf` | SOCOM: Combined Assault | measured on this title |
| `SLUS-20883.conf` | Rainbow Six 3 | measured on this title |
| `SCUS-97134.conf` | SOCOM: U.S. Navy SEALs | sky and `MINRT` measured here, rest transplanted |
| `SCUS-97275.conf` | SOCOM II | transplanted, **unmeasured on this title** |
| `SCUS-97474.conf` | SOCOM 3 | starter profile, **unmeasured** |
| `SCUS-97399.conf` | God of War | starter profile, **unmeasured**, different engine |

They're worth reading even if you don't play those games: they record *why* each setting is what it is, with the measurement behind it. None of them arms a diagnostic.

**This build is portable.** `portable.ini` is present, so settings, memory cards and save states stay in the folder instead of `Documents\PCSX2` — installing it next to a normal PCSX2 cannot overwrite that install's config. `updater.exe` is deliberately absent: it updates to upstream PCSX2 and would replace this build with one that has no Remix backend in it.

---

## What works

| Title | Serial | State |
|---|---|---|
| Ghost Recon 2 | `SLUS-21105` | World geometry, albedo textures, generated vertex normals, path-traced lighting -- **not re-verified since**, see below |
| Rainbow Six 3 | `SLUS-20883` | The above plus character models, EE-memory camera, shadow pass dropped, baked lightmaps folded in |
| SOCOM: Combined Assault | `SCUS-97545` | **Being worked on, not playable.** See below. |

**On Ghost Recon 2:** nothing is known to be wrong with it, but its capture is 40 commits behind this build and nobody has booted it since. The blend-enum fix and the sprite-text gate both landed after that capture and both change what every title submits, so read that row as "was working, unverified against this build" rather than as a current result. Rainbow Six 3 is the exception -- its camera, shadow-pass and lightmap work was measured on this tip.

**On SOCOM specifically:** geometry, textures and characters were verified on 2026-08-02, but that is 83 commits back and it has not been re-measured since. It is actively being worked on rather than parked — 18 of those 83 commits touch this title, including the render-target gate, the sky classifier and the empty-window fix — but the device loss below still ends a session early. Do not download this expecting to play SOCOM.

The interesting part remains the camera. The PS2 has no fixed-function transform unit, so there is no `SetTransform` to read a view matrix out of — the view-projection lives inside a VU1 microprogram the game uploaded, and the backend back-slices it out of the microcode. [Write-up in the README.](https://github.com/BRAGme/pcsx2-rtx-remix#how-the-camera-is-recovered)

---

## What does not work

**One camera per frame, applied to every draw — by default.** The backend picks a single view-projection from the frame's candidates and uses it for the whole frame. Per-draw placement *is* built (`PCSX2_REMIX_PERDRAWCAM`) and the knob is live, so it can be switched on mid-run with one `.conf` line; it ships off deliberately, measurement before behaviour. On Rainbow Six 3 save state 9, 99.84% of frames anchor to world space — but on save state 7, which runs a different VU1 microprogram, three runs measured 35.3%, 48.4% and 68.3%. Applying the wrong inverse is a projective error, not a rigid one: near vertices barely move, far vertices swing hard, and a triangle spanning depth becomes a spike. On SOCOM CA per-draw placement would not help anyway — `sol = 1` on all 114 non-empty windows measured.

Partly superseded on Rainbow Six 3 only, by the EE-memory camera above. Every other title still uses the VU1 recovery path. Worth knowing why that path can fail outright: `VIFMAP`, a write-side probe over 7.8M VIF1 unpacks, found 360 distinct upload shapes and **none** that writes a standalone matrix once per frame — the EE composes per-object MVPs and ships only the product. If a title resists the VU1 path, that is the first thing to check.

**Reproducible device loss (`0x60D0DEAD`) on some titles.** On SOCOM the hang lands within a fraction of a second of the renderer going live, and loading heavy mission geometry is what triggers it. Not tuneable from the outside — four different entry strategies were measured and the ceiling is the crash, not the navigation.

**Misplaced geometry on untested titles.** The two known instances were root-caused and fixed, not mitigated. SOCOM's geometry scatter was a 128x128 render-to-texture pass submitted as world geometry; `PCSX2_REMIX_MINRT` rejects those and took maxpos from 151,966 to 14,613 — but **it defaults to 0 and is set per game**, so a title without a shipped `.conf` can still show it. The white shards were a separate fault (blended draws reaching Remix as opaque against a 4x4 white untextured material, 210 of 1,874 draws on SOCOM Winterblade) and `ALPHASTATE` now defaults to 2, so that one is fixed everywhere. "Vertex explosions" was measured and rejected as the explanation for either: `PCSX2_REMIX_EXPLODEK` peaked at 3.7x against a 32x limit while the screen was visibly in pieces.

**Only the titles above have been measured.** The four transplanted `.conf` files have not been tested on their own titles. Anything else is untested, which is not the same as broken.

---

## Building it yourself

If you'd rather compile: a fresh clone **will not build** until you supply the third-party dependencies, and the failure looks like a broken repo rather than a missing step — a wall of `C1083` misses across `zlib.h`, `zstd.h`, `ft2build.h`, `jpeglib.h`, `ryml.hpp` and `directx/d3d12.h` all at once. MSBuild resolves all of them from `$(SolutionDir)deps\`, which is gitignored because it is obtained, not committed.

Fastest fix, verified on a clean clone: download `pcsx2-windows-dependencies.7z` from [PCSX2/pcsx2-windows-dependencies](https://github.com/PCSX2/pcsx2-windows-dependencies/releases/tag/latest-windows-dependencies) and extract it **at the repo root** — the archive already contains a top-level `deps` folder. Then build `PCSX2_qt.sln` normally.

There are no git submodules. `.gitmodules` is empty and upstream moved every vendored library in-tree, so `git submodule update` will appear to do nothing. That is correct, not a failed clone. Full instructions in the [README](https://github.com/BRAGme/pcsx2-rtx-remix#building).

---

PCSX2 is GPL-3.0. This is a modified fork; the complete corresponding source is this repository.

---

`pcsx2-rtx-remix-e89ed7295-win64.zip`
SHA256 `5b8ab5a985b98b2d7c67d87ba831ca667b1b021483ab9fee2795a596d13418ba`

Built from `remix-backend` commit `e89ed7295`, tagged `remix-preview-2`. Reports `remix-preview-2` in Help -> About, so you can check.
