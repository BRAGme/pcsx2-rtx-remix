**PCSX2 RTX Remix — preview 3**
<https://github.com/BRAGme/pcsx2-rtx-remix/releases/tag/remix-preview-3>

Config fix. **The emulator code is byte-identical to preview 2** — no C++ changed between the tags.

🔧 What was wrong:
• Preview 2 shipped the Rainbow Six 3 work without the config that switches it on — the config predated the features by a day and never got updated
• `EECAM` and `SHADOWPASS` sat at 0, and the FOV sat at 48.1, the value already *refuted* in favour of 70 (lamp-to-fixture error 14.0 → 1.4 units)
• So preview 2 on Rainbow Six 3 gave you the old VU1 camera, the ghost bodies through walls, and a projection known to be wrong

✅ Now on Rainbow Six 3:
• The camera comes out of EE memory instead of being back-sliced from a VU1 microprogram
• The shadow pass is dropped, so the duplicate bodies drifting through walls are gone
• The projection uses the measured FOV

⚠️ Worth knowing:
• **If you don't play Rainbow Six 3 this changes nothing for you** — same binary, more honest docs
• Preview 2's notes were wrong in five places and are corrected in place, all in the same direction: per-title fixes described as global
• Ghost Recon 2 now carries the same "not re-verified" caveat SOCOM has — its screenshot is 40 commits old
• SOCOM: Combined Assault is still not playable, and nothing here is playable start to finish

🤝 The repo is open now:
• Issues are enabled, and a per-game `.conf` is the most useful thing you can send
• No git needed — paste it into an issue and you get credited as co-author
• AI-assisted work is welcome and needs no disclosure
• One ask: only write down numbers you measured on that game, and label the rest — a model filling in a config invents measurements that look exactly like real ones
