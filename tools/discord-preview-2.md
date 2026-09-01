**PCSX2 RTX Remix — preview 2**
<https://github.com/BRAGme/pcsx2-rtx-remix/releases/tag/remix-preview-2>

✅ Fixed:
• Surfaces looked flat and washed out because the texture stage was misconfigured — a D3D9-vs-Remix enum mismatch had colour coming from vertex colour at double brightness
• The flickering "z-fighting" triangles were an empty present window, not depth precision
• PS2 text is drawn as sprites and the classifier was throwing all of it away
• USD capture never worked for anyone — a locale bug was making Remix's prim names invalid
• Rainbow Six 3's camera now comes straight from EE memory, its shadow pass is dropped and its baked lightmaps are folded in
• Sky is classified by depth instead of draw order
• Six per-game configs, up from two
• Building from source works — a fresh clone used to fail on every third-party lib at once

⚠️ Issues:
• SOCOM: Combined Assault is not playable, device loss still ends the session early
• Ghost Recon 2 hasn't been re-checked in 40 commits, so treat its screenshot as old
• One camera per frame, so on some titles geometry welds to the screen
• Geometry can still land in the wrong place on an untested title — the render-target gate that fixes it (MINRT) is off by default and set per game
• Only three titles have been measured *at all* — anything else is untested, not broken
• Needs a Remix Plus / extended-API-line runtime (stock Remix won't connect) plus your own BIOS
