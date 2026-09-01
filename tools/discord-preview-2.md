**PCSX2 RTX Remix — preview 2 is up**
<https://github.com/BRAGme/pcsx2-rtx-remix/releases/tag/remix-preview-2>

Three weeks on from preview 1. The headline: **the flat, untextured look was an enum bug, not texture binding.** D3D9 enum values were landing in fields the runtime casts straight to Vulkan enums — `textureColorArg1Source = 2` meant "use the texture" to us and "vertex colour" to Remix, so the backend ignored textures entirely. `MODULATE` also read as `Modulate2x`, doubling brightness.

**Also fixed**
- **Flickering / "z-fighting" triangles** — not depth precision. A game running below the vsync rate submits zero geometry on every Nth present, and that empty frame got presented. Now held as a true duplicate.
- **PS2 text was invisible** — it's drawn as sprites, and the first classifier gate discarded all of it.
- **USD capture never worked, for anyone** — PCSX2 sets a grouping locale, so Remix's hex prim names came out comma-separated, which is invalid USD. Every capture died 18 ms in.
- **Rainbow Six 3** — camera read straight from EE memory, shadow pass dropped (those duplicate bodies drifting through walls), baked lightmaps folded in, and FOV finally calibrated: lamp-to-fixture error 14.0 → **1.4 units**.
- Sky classified by depth, not draw order; render-to-texture passes no longer submitted as world geometry.
- **Six per-game configs**, up from two.
- **Building it works now** — a fresh clone used to fail on every third-party lib at once. README points at the prebuilt deps.

**Straight about the state** — still a research build; nothing is playable start to finish.
- **SOCOM: Combined Assault is not playable** — device loss still ends a session early. Actively worked on, not parked.
- **Ghost Recon 2's screenshot is 40 commits behind this build** and hasn't been re-checked.
- **Rainbow Six 3** is the one title measured against this actual build.

Needs a Remix Plus / extended-API-line runtime (stock Remix won't connect) plus your own BIOS.
