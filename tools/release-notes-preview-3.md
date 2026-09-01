A config and documentation fix. **The emulator code is byte-identical to preview 2** — no C++ changed between the two tags. If you do not play Rainbow Six 3, the only thing this gives you is a more honest README.

---

## Why this exists

**Preview 2 shipped the Rainbow Six 3 backend work without the config that switches it on.**

`bin/SLUS-20883.conf` was last updated on 2026-08-27. The features landed after it — the EE-memory camera on 08-28, the projection calibration and shadow-pass drop on 08-29. So the profile that shipped set none of them, and every knob fell back to its compiled default:

| Setting | Preview 2 shipped | Should be |
|---|---|---|
| `EECAM` | `0` — off | `2` |
| `EECAMLOC` / `EECAMROT` | `0` / `0` | the two EE addresses |
| `EECAMFOV` | **`48.1`** | `70.0` |
| `SHADOWPASS` | `0` — off | `1` |

That `48.1` is not merely untuned, it is the value that was specifically *refuted*: `LIGHTFIT` measured a lamp-to-fixture residual of 14.0 units at fov 60 against **1.4 at fov 70**, which is what replaced it.

So anyone who ran preview 2 on Rainbow Six 3 got the VU1 camera path, the ghost bodies drifting through walls, and a projection already known to be wrong — while the release notes described the opposite. That is fixed here, and preview 2's notes now carry a correction saying so rather than leaving the claim standing.

## What you get on Rainbow Six 3 now

- **The camera comes out of EE memory** rather than being back-sliced from a VU1 microprogram. It was never in VU1 on this title: a write-side probe over 7.8M VIF1 unpacks found 360 distinct upload shapes and none that writes a standalone matrix once per frame, because the EE composes per-object MVPs and ships only the product.
- **The shadow pass is dropped.** The game draws character shadows by re-rendering the character from the light's viewpoint into a 512×256 target; un-projected with the player's camera each becomes a duplicate untextured body drifting through walls. Measured over 708 dumped draws: 9 draws with `ZTST=1` + `ZMSK=1`, 1.3% of draws carrying 24% of the vertices.
- **The projection is calibrated** — fov 70, from the lamp-to-fixture measurement above.

The addresses are title- and build-specific (`0xF0F670` / `0xF0F680`, for SLUS-20883) and the config says so. Do not carry them to another game or another region's disc without re-deriving them.

---

## The notes for preview 2 were wrong in five places

Every error was in the same direction — describing a per-title fix as though it were global, or claiming more than the shipped defaults deliver. Corrected in the README and in preview 2's own notes:

- **"Textures were being ignored entirely."** The enum mechanism was real, but all three preview-1 screenshots were captured with those exact values and plainly show texture detail. The symptom was flat and washed out, not absent.
- **"Vertex explosions / white shards" listed as an open blocker.** Both known instances were root-caused and fixed. The white shards were `ALPHASTATE = 0` against a 4×4 white untextured material (now defaults to 2, fixed everywhere); the geometry scatter was a render-to-texture pass (`MINRT`). "Vertex explosions" was measured and *rejected* as the cause of either — `EXPLODEK` peaked at 3.7× against a 32× limit while the screen was visibly in pieces.
- **"Per-draw camera association is not built."** It is built. `PCSX2_REMIX_PERDRAWCAM` is live and can be switched on mid-run with one config line; it ships off deliberately, measurement before behaviour.
- **The flicker fix described as global.** `HOLDEMPTY` rides the batch path and is inert unless a title also sets `BATCH = 1` — four profiles do, Rainbow Six 3 does not.
- **Sky-by-depth described as global.** It needs `SKYMINW`, which defaults to 0 and is set in two profiles.

Ghost Recon 2 also now carries the same "not re-verified" caveat SOCOM has: nothing is known to be wrong with it, but its capture is 40 commits behind and the blend-enum fix landed after it.

---

## The repo is open to contributions now

Issues are enabled, with templates for [a bug](https://github.com/BRAGme/pcsx2-rtx-remix/issues/new?template=remix_bug_report.yaml) and for [a per-game config](https://github.com/BRAGme/pcsx2-rtx-remix/issues/new?template=game_config.yaml).

**A `.conf` is the most useful thing you can send.** It touches no code, cannot break another title, and the packager ships `bin/*.conf` automatically. You do not need git — paste it into an issue and it gets committed with you credited as co-author.

AI-assisted work is welcome and needs no disclosure; much of this fork was written that way. The one ask is in [CONTRIBUTING.md](https://github.com/BRAGme/pcsx2-rtx-remix/blob/remix-backend/CONTRIBUTING.md): only write down numbers you actually measured on that title, and label the rest. A model asked to fill in a config will invent measurements that look exactly like real ones — that has already happened in this repo twice.

---

## Everything else is unchanged from preview 2

The runtime requirement, the BIOS requirement, what works and what does not, and the build instructions are all as they were. **[Read preview 2's notes](https://github.com/BRAGme/pcsx2-rtx-remix/releases/tag/remix-preview-2)** for those — they are not repeated here.

Short version: you need a dxvk-remix fork on the `0.1000.x` API line (stock Remix will not connect) and your own BIOS. SOCOM: Combined Assault is still not playable. Nothing here is playable start to finish.

---

`pcsx2-rtx-remix-b02a5d5f7-win64.zip`
SHA256 `90d5d6b1397e49170030d82db3f9bd50fe8f8d88fa4dd4e49b6f8d6ba5abf102`

Built from `remix-backend` commit `b02a5d5f7`, tagged `remix-preview-3`. Reports `remix-preview-3` in Help -> About.
