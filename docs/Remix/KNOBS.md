# RTX Remix backend knobs

This file is GENERATED from the knob table in `pcsx2/GS/Remix/RemixKnobs.cpp`. Do not hand-edit it
-- if the table changes, re-generate this file from the table so the two cannot disagree.

A knob can be set three ways, all of which name the same thing:

- the environment variable `PCSX2_REMIX_<env>`,
- the settings key `[Remix]/<env>` in `bin/inis/PCSX2.ini`,
- the RTX Remix settings page in the GUI.

Knobs marked latched are read once at backend start, through a `static const` local, so the value is
captured the first time that code runs and never re-read. Changing one takes effect on the next
restart, and the settings page greys it out while a game is running to say so. Every other knob is
re-read, and the bridge re-applies it about once a second, so it responds while the game runs.

Boolean knobs are stored as `0` / `1`. For Choice knobs the range column lists the combo entries in
value order starting at 0.

## Scene and Scale

| env | type | default | range | latched | what it does |
| --- | --- | --- | --- | --- | --- |
| `PCSX2_REMIX_CAMSCALE` | Float | 8.0 | 0.01 .. 1000.0 | yes | Scale applied turning PS2 units into Remix world units. Sets how big the scene is to lights and to the path tracer. |
| `PCSX2_REMIX_NEARPLANE` | Float | 1.0 | 0.001 .. 10000.0 | yes | Near clip distance used when rebuilding the projection. |
| `PCSX2_REMIX_POSLIMIT` | Float | 400000.0 | 0.0 .. 1e9 |  | Vertices further than this from the origin are dropped. Catches the runaway positions that show up as vertex explosions. |

## Lighting

| env | type | default | range | latched | what it does |
| --- | --- | --- | --- | --- | --- |
| `PCSX2_REMIX_LIGHTMODE` | Choice | 1 | Off (game lights only)\|Dome and key light (no distance falloff)\|Point light at the camera |  | How the scene is lit when the game's own lights are not reconstructed. A dome and a distant light have no distance falloff, so one brightness works whether the scene is a corridor or an outdoor map; a point light at the camera does not. |
| `PCSX2_REMIX_KEY` | Float | 100.0 | 0.0 .. 100000.0 |  | Radiance of the key light. |
| `PCSX2_REMIX_KEYANGLE` | Float | 8.0 | 0.1 .. 180.0 |  | Angular diameter of the key light. Smaller is sharper shadows. |
| `PCSX2_REMIX_AMBIENT` | Float | 0.0 | 0.0 .. 100000.0 |  | Radiance of the ambient dome. Raise to lift shadows that read as black. |

## Camera

| env | type | default | range | latched | what it does |
| --- | --- | --- | --- | --- | --- |
| `PCSX2_REMIX_NOCAM` | Boolean | 0 | - | yes | Submit in view space instead of recovering a world camera. Diagnostic. |
| `PCSX2_REMIX_CAMDIST` | Float | 50.0 | 0.0 .. 100000.0 | yes | Rejects a recovered camera that jumped further than this in one frame. |
| `PCSX2_REMIX_CAMEXTENT` | Float | 10.0 | 0.0 .. 100000.0 | yes | Rejects a camera whose basis vectors imply an implausible scene extent. |
| `PCSX2_REMIX_CAMANISO` | Float | 8.0 | 1.0 .. 1000.0 | yes | Rejects a camera basis more anisotropic than this. Guards against shears. |
| `PCSX2_REMIX_STARTUPDELAY` | Integer | 0 | 0 .. 100000 |  | Frames to wait before submitting anything. Lets a boot sequence settle. |
| `PCSX2_REMIX_SUBMITDELAY` | Integer | 0 | 0 .. 100000 |  | Frames to wait after the camera solves before submitting geometry. |

## Sky

| env | type | default | range | latched | what it does |
| --- | --- | --- | --- | --- | --- |
| `PCSX2_REMIX_SKY` | Choice | 1 | Off\|Depth-neutral draws\|First N draws (needs Sky draw order) |  | Tags the skybox so Remix renders it at infinity instead of as geometry a few feet in front of the camera. "Depth-neutral" catches a sky drawn with Z testing and Z writing both off. If the title's sky tests Z, that test can never match it -- use "First N draws" and set Sky draw order, which is how dxvk-remix does it natively. Neither test is needed if you tag the sky texture by hand in the Remix developer menu; that is exact, and it applies without a restart. |
| `PCSX2_REMIX_SKYORDER` | Integer | 0 | 0 .. 100000 |  | How many leading draws in a frame are eligible to be classified as sky. Narrows "Depth-neutral draws" (0 = no limit); required by "First N draws", which does nothing while this is 0. |

## Textures and Materials

| env | type | default | range | latched | what it does |
| --- | --- | --- | --- | --- | --- |
| `PCSX2_REMIX_TEXSTAGE` | Choice | 1 | Send nothing (let Remix decide)\|Derive from the PS2 TFX mode |  | Whether to send Remix explicit fixed-function texture-stage state -- which argument the stage samples and how it combines with the vertex colour. Deriving it from TFX is the faithful translation; sending nothing falls back to Remix's own default, which is what to try if surfaces render flat and ignore their texture. |
| `PCSX2_REMIX_MATSTAGE` | Integer | 4 | 1 .. 4 |  | Texture stage the material is built from. |
| `PCSX2_REMIX_REPLACEALBEDO` | Boolean | 0 | - | yes | Binds albedo to a matching .dds from PCSX2's own texture replacement pack when one exists for that texture hash. |
| `PCSX2_REMIX_TEXALPHA` | Boolean | 1 | - | yes | Keeps the decoded alpha channel instead of forcing opaque. |
| `PCSX2_REMIX_TEXLINEAR` | Boolean | 0 | - | yes | Uploads textures as linear rather than sRGB. |
| `PCSX2_REMIX_TEXREUPLOAD` | Boolean | 1 | - | yes | Re-uploads texture data when a material is rebuilt. |
| `PCSX2_REMIX_MATREBUILD` | Integer | 0 | 0 .. 100000 |  | Frames between re-resolving a material's texture. 0 (default) disables it. Leave it off: a rebuild swaps the material handle without invalidating cached meshes, so with stable mesh identity on, meshes keep the old handle until it is destroyed and their surfaces go white on a timer. Diagnostic only. |
| `PCSX2_REMIX_TEXBUDGET` | Integer | 32 | 0 .. 4096 |  | Cap on new texture uploads per frame. Raising it far above the default has been measured to lose the device outright. |
| `PCSX2_REMIX_TEXCAP` | Integer | 4096 | 16 .. 65536 |  | Maximum live Remix textures before the least recently used are released. |
| `PCSX2_REMIX_TEXIDLE` | Integer | 600 | 1 .. 100000 |  | Frames a texture may go unused before it is eligible for release. |

## Emissive and Lightmaps

| env | type | default | range | latched | what it does |
| --- | --- | --- | --- | --- | --- |
| `PCSX2_REMIX_LIGHTMAPS` | Boolean | 1 | - |  | Reports the texture hashes used by the game's lightmap passes. |
| `PCSX2_REMIX_LIGHTMAPINJECT` | Boolean | 0 | - |  | Submits the masked lightmap passes as extra surfaces. Measured to render as black flickering squares on Rainbow Six 3 -- off by default. |

## Geometry and Filtering

| env | type | default | range | latched | what it does |
| --- | --- | --- | --- | --- | --- |
| `PCSX2_REMIX_SMOOTHNORMALS` | Integer | 0 | 0 .. 180 |  | Generates smooth vertex normals instead of one flat normal per triangle, so curved surfaces stop reading as faceted plates -- the visible polygons on character faces. The value is a crease angle in degrees: a corner is smoothed only if its own face lies within that angle of the average, so 60 rounds off a face while leaving a 90 degree building corner sharp. 0 keeps flat normals. The PS2 sends no normals of its own, so nothing in the Remix developer menu can substitute for this. |
| `PCSX2_REMIX_UNTEXZ` | Boolean | 1 | - |  | Routes untextured draws through the Z-to-w calibration and submits them. They are the majority of a frame in some games. |
| `PCSX2_REMIX_VCOLOR` | Boolean | 1 | - |  | Passes the PS2 per-vertex colour through to Remix. |
| `PCSX2_REMIX_VCBAKED` | Boolean | 0 | - |  | Tells Remix the vertex colour is baked lighting rather than albedo, so it is not multiplied into the material twice. |
| `PCSX2_REMIX_CUTOUT` | Boolean | 1 | - |  | Classifies alpha-tested draws as cutouts so foliage resolves correctly. |
| `PCSX2_REMIX_ALPHASTATE` | Integer | 2 | 0 .. 3 |  | How much of the PS2 alpha/blend state is forwarded to Remix. |
| `PCSX2_REMIX_WFLAT` | Float | 0.001 | 0.0 .. 1.0 |  | Rejects a draw whose w values are this close together, which is how a 2D overlay looks. Raise to drop more 2D, lower to keep more. |
| `PCSX2_REMIX_MINW` | Float | 0.01 | 0.0 .. 1000.0 |  | Draws with a smaller maximum w are skipped. |
| `PCSX2_REMIX_MINVW` | Float | 0.0 | 0.0 .. 1000.0 |  | Skips a draw any of whose vertices falls below this w, rather than one whose furthest vertex does. A draw can span a fine far end and a degenerate near end, and "Minimum w" passes it untouched. At very small w there is no precision left to reconstruct a world position from, which is how a surface ends up pinned in front of the view and staying there as you turn. 0 disables it. Raise it in small steps -- the first-person weapon genuinely sits near the eye, so too high a value removes that too. |
| `PCSX2_REMIX_MAXW` | Float | 0.0 | 0.0 .. 100000.0 |  | Draws beyond this w are skipped. 0 disables the gate. |
| `PCSX2_REMIX_EXPLODEK` | Float | 32.0 | 0.0 .. 100000.0 |  | Rejects a draw whose screen extent exceeds this multiple of its w, the signature of a vertex explosion. 0 disables it. |
| `PCSX2_REMIX_FSTZ` | Boolean | 1 | - |  | Recovers depth for draws that use FST texture coordinates. |
| `PCSX2_REMIX_FSTFLAT` | Boolean | 0 | - |  | Allows FST recovery on draws whose Z barely varies. |

## Mesh Identity

| env | type | default | range | latched | what it does |
| --- | --- | --- | --- | --- | --- |
| `PCSX2_REMIX_STABLEID` | Boolean | 0 | - |  | Matches this frame's meshes against the previous frame by rigid registration instead of hashing positions. |
| `PCSX2_REMIX_IDCOLOR` | Boolean | 1 | - | yes | Folds vertex colour into the mesh hash. |
| `PCSX2_REMIX_IDSLOTS` | Integer | 4 | 1 .. 64 |  | Candidate slots considered when matching a mesh to a previous one. |
| `PCSX2_REMIX_MESHCAP` | Integer | 4096 | 16 .. 65536 |  | Maximum live Remix meshes before the least recently used are released. |
| `PCSX2_REMIX_MESHIDLE` | Integer | 120 | 1 .. 100000 |  | Frames a mesh may go unused before it is eligible for release. |
| `PCSX2_REMIX_MESHBUDGET` | Integer | 0 | 0 .. 100000 |  | Cap on new meshes per frame. 0 is unlimited. |
| `PCSX2_REMIX_INSTBUDGET` | Integer | 0 | 0 .. 1000000 |  | Cap on instances submitted per frame. 0 is unlimited. |
| `PCSX2_REMIX_REUSEHANDLE` | Boolean | 0 | - | yes | Reuses a mesh handle when the geometry is judged unchanged. |
| `PCSX2_REMIX_REUSEPOOL` | Integer | 0 | 0 .. 65536 |  | Size of the pool of handles held for reuse. 0 disables pooling. |
| `PCSX2_REMIX_BATCH` | Boolean | 0 | - |  | Merges compatible draws into one mesh before submitting. |
| `PCSX2_REMIX_BATCHRETAIN` | Integer | 3 | 0 .. 1000 |  | Frames a batch group is kept before being rebuilt. |

## Diagnostics

| env | type | default | range | latched | what it does |
| --- | --- | --- | --- | --- | --- |
| `PCSX2_REMIX_DRAWDUMP` | Integer | 0 | 0 .. 100000 |  | Writes remix_draws.txt describing every submitted draw for N frames. |
| `PCSX2_REMIX_TEXDUMP` | Boolean | 1 | - | yes | Logs each decoded texture's hash and mean colour. |
| `PCSX2_REMIX_FBMSKDUMP` | Boolean | 0 | - |  | Logs draws rejected by the FBMSK gate. |
| `PCSX2_REMIX_ALBEDOPROBE` | Boolean | 0 | - | yes | Fills every uploaded texture with magenta. Anything still white is not sampling our texture at all. |
| `PCSX2_REMIX_STATSFRAMES` | Integer | 300 | 1 .. 100000 |  | How often the counter block is written to the log. |
| `PCSX2_REMIX_SCANKICKS` | Integer | 16 | 0 .. 4096 |  | How many VU1 kicks are scanned per frame when recovering the camera. |
| `PCSX2_REMIX_NODEBUGSCENE` | Boolean | 0 | - | yes | Suppresses the built-in debug geometry. |
| `PCSX2_REMIX_NODRAWINSTANCE` | Boolean | 0 | - | yes | Builds meshes but submits no instances. Isolates cost between the two. |
| `PCSX2_REMIX_SPIKE` | Boolean | 0 | - | yes | Logs frames that took unusually long. |
</content>
</invoke>
