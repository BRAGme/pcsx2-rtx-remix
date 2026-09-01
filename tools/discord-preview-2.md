**PCSX2 RTX Remix — preview 2**
<https://github.com/BRAGme/pcsx2-rtx-remix/releases/tag/remix-preview-2>

✅ Fixed:
• Surfaces looked flat and washed out because the texture stage was misconfigured — a D3D9-vs-Remix enum mismatch had colour coming from vertex colour at double brightness
• The flickering "z-fighting" triangles were an empty present window, not depth precision — active on the SOCOM/GoW profiles, which set the BATCH mode it rides on
• PS2 text is drawn as sprites and the classifier was throwing all of it away
• USD capture was broken on any system with digit grouping — a locale bug made Remix's hex prim names comma-separated, which is invalid USD
• Rainbow Six 3 gets an EE-memory camera, its shadow pass dropped and a calibrated FOV — **the code is in preview 2 but its config was not, so this only lands in the next build**
• Sky is classified by depth instead of draw order, on the two SOCOM profiles that set the threshold
• Six per-game configs, up from two
• Building from source works — a fresh clone used to fail on every third-party lib at once

⚠️ Issues:
• SOCOM: Combined Assault is not playable, device loss still ends the session early
• Ghost Recon 2 hasn't been re-checked in 40 commits, so treat its screenshot as old
• One camera per frame by default, which shatters geometry that spans depth — per-draw placement is built and live-tunable, it just ships off
• Geometry can still land in the wrong place on an untested title — the render-target gate that fixes it (MINRT) is off by default and set per game
• Only three titles have been verified as rendering — anything else is untested, not broken
• Needs a Remix Plus / extended-API-line runtime (stock Remix won't connect) plus your own BIOS
