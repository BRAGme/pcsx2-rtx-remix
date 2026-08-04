// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Remix/RemixKnobs.h"

#include <iterator>

namespace remix_ps2
{
	namespace
	{
		// Defaults here are the SAME values the backend falls back to when the variable is unset.
		// They were lifted from the read sites rather than retyped from memory; if one drifts, the
		// GUI would show a default the backend does not actually use.
		//
		// `latched` was determined the same way: a read assigned to a `static const` local is
		// captured once and never re-read.
		const knob s_knobs[] = {
			// ------------------------------------------------------------------ Scene and Scale
			{"CAMSCALE", "Scene and Scale", "World scale", knob_type::Float, 8.0, 0.01, 1000.0, 0.5,
				nullptr,
				"Scale applied turning PS2 units into Remix world units. Sets how big the scene is "
				"to lights and to the path tracer.",
				true},
			{"NEARPLANE", "Scene and Scale", "Near plane", knob_type::Float, 1.0, 0.001, 10000.0, 0.5,
				nullptr, "Near clip distance used when rebuilding the projection.", true},
			{"POSLIMIT", "Scene and Scale", "Position limit", knob_type::Float, 400000.0, 0.0, 1e9, 1000.0,
				nullptr,
				"Vertices further than this from the origin are dropped. Catches the runaway "
				"positions that show up as vertex explosions.",
				false},

			// ---------------------------------------------------------------------- Lighting
			{"LIGHTMODE", "Lighting", "Lighting mode", knob_type::Choice, 1, 0, 2, 1,
				"Off (game lights only)|Dome and key light (no distance falloff)|Point light at the camera",
				"How the scene is lit when the game's own lights are not reconstructed. A dome and a "
				"distant light have no distance falloff, so one brightness works whether the scene "
				"is a corridor or an outdoor map; a point light at the camera does not.",
				false},
			{"KEY", "Lighting", "Key light brightness", knob_type::Float, 100.0, 0.0, 100000.0, 5.0,
				nullptr, "Radiance of the key light.", false},
			{"KEYANGLE", "Lighting", "Key light angular size", knob_type::Float, 8.0, 0.1, 180.0, 1.0,
				nullptr, "Angular diameter of the key light. Smaller is sharper shadows.", false},
			{"AMBIENT", "Lighting", "Ambient brightness", knob_type::Float, 0.0, 0.0, 100000.0, 1.0,
				nullptr, "Radiance of the ambient dome. Raise to lift shadows that read as black.", false},

			// --------------------------------------------------------------------- Camera
			{"NOCAM", "Camera", "Disable camera recovery", knob_type::Boolean, 0, 0, 1, 1, nullptr,
				"Submit in view space instead of recovering a world camera. Diagnostic.", true},
			{"CAMDIST", "Camera", "Camera distance limit", knob_type::Float, 50.0, 0.0, 100000.0, 5.0,
				nullptr, "Rejects a recovered camera that jumped further than this in one frame.", true},
			{"CAMEXTENT", "Camera", "Camera extent limit", knob_type::Float, 10.0, 0.0, 100000.0, 1.0,
				nullptr, "Rejects a camera whose basis vectors imply an implausible scene extent.", true},
			{"CAMANISO", "Camera", "Camera anisotropy limit", knob_type::Float, 8.0, 1.0, 1000.0, 1.0,
				nullptr, "Rejects a camera basis more anisotropic than this. Guards against shears.", true},
			{"STARTUPDELAY", "Camera", "Startup delay (frames)", knob_type::Integer, 0, 0, 100000, 10,
				nullptr, "Frames to wait before submitting anything. Lets a boot sequence settle.", false},
			{"SUBMITDELAY", "Camera", "Submit delay (frames)", knob_type::Integer, 0, 0, 100000, 10,
				nullptr, "Frames to wait after the camera solves before submitting geometry.", false},

			// ------------------------------------------------------------------------ Sky
			{"SKY", "Sky", "Sky classification", knob_type::Choice, 1, 0, 2, 1,
				"Off|Depth-neutral draws|First N draws (needs Sky draw order)",
				"Tags the skybox so Remix renders it at infinity instead of as geometry a few feet "
				"in front of the camera. \"Depth-neutral\" catches a sky drawn with Z testing and Z "
				"writing both off. If the title's sky tests Z, that test can never match it -- use "
				"\"First N draws\" and set Sky draw order, which is how dxvk-remix does it natively. "
				"Neither test is needed if you tag the sky texture by hand in the Remix developer "
				"menu; that is exact, and it applies without a restart.",
				false},
			{"SKYORDER", "Sky", "Sky draw order", knob_type::Integer, 0, 0, 100000, 1, nullptr,
				"How many leading draws in a frame are eligible to be classified as sky. Narrows "
				"\"Depth-neutral draws\" (0 = no limit); required by \"First N draws\", which does "
				"nothing while this is 0.",
				false},

			// ------------------------------------------------------- Textures and Materials
			// Ranges here MUST match the clamp at the read site. These two offered 0-8 while the
			// backend clamped to 0-1 and 1-4, so seven of the nine choices silently collapsed onto
			// another and the page reported a setting the backend was not using.
			{"TEXSTAGE", "Textures and Materials", "Texture stage state", knob_type::Choice, 1, 0, 1, 1,
				"Send nothing (let Remix decide)|Derive from the PS2 TFX mode",
				"Whether to send Remix explicit fixed-function texture-stage state -- which argument "
				"the stage samples and how it combines with the vertex colour. Deriving it from TFX "
				"is the faithful translation; sending nothing falls back to Remix's own default, "
				"which is what to try if surfaces render flat and ignore their texture.",
				false},
			{"MATSTAGE", "Textures and Materials", "Material stage", knob_type::Integer, 4, 1, 4, 1,
				nullptr, "Texture stage the material is built from.", false},
			{"REPLACEALBEDO", "Textures and Materials", "Use replacement textures", knob_type::Boolean, 0, 0, 1, 1,
				nullptr,
				"Binds albedo to a matching .dds from PCSX2's own texture replacement pack when one "
				"exists for that texture hash.",
				true},
			{"TEXALPHA", "Textures and Materials", "Honour texture alpha", knob_type::Boolean, 1, 0, 1, 1,
				nullptr, "Keeps the decoded alpha channel instead of forcing opaque.", true},
			{"TEXLINEAR", "Textures and Materials", "Linear texture filtering", knob_type::Boolean, 0, 0, 1, 1,
				nullptr, "Uploads textures as linear rather than sRGB.", true},
			{"TEXREUPLOAD", "Textures and Materials", "Re-upload on rebuild", knob_type::Boolean, 1, 0, 1, 1,
				nullptr, "Re-uploads texture data when a material is rebuilt.", true},
			{"MATREBUILD", "Textures and Materials", "Material rebuild interval", knob_type::Integer, 0, 0, 100000, 10,
				nullptr, "Frames between rebuilding a material. 0 disables rebuilding.", false},
			{"TEXBUDGET", "Textures and Materials", "Texture uploads per frame", knob_type::Integer, 32, 0, 4096, 8,
				nullptr,
				"Cap on new texture uploads per frame. Raising it far above the default has been "
				"measured to lose the device outright.",
				false},
			{"TEXCAP", "Textures and Materials", "Texture cache size", knob_type::Integer, 4096, 16, 65536, 256,
				nullptr, "Maximum live Remix textures before the least recently used are released.", false},
			{"TEXIDLE", "Textures and Materials", "Texture idle frames", knob_type::Integer, 600, 1, 100000, 60,
				nullptr, "Frames a texture may go unused before it is eligible for release.", false},

			// ---------------------------------------------------------------------- Emissive
			{"LIGHTMAPS", "Emissive and Lightmaps", "Discover lightmaps", knob_type::Boolean, 1, 0, 1, 1,
				nullptr, "Reports the texture hashes used by the game's lightmap passes.", false},
			{"LIGHTMAPINJECT", "Emissive and Lightmaps", "Inject lightmaps", knob_type::Boolean, 0, 0, 1, 1,
				nullptr,
				"Submits the masked lightmap passes as extra surfaces. Measured to render as black "
				"flickering squares on Rainbow Six 3 -- off by default.",
				false},

			// ----------------------------------------------------------- Geometry and Filtering
			{"SMOOTHNORMALS", "Geometry and Filtering", "Smooth normals (crease angle)",
				knob_type::Integer, 0, 0, 180, 5, nullptr,
				"Generates smooth vertex normals instead of one flat normal per triangle, so curved "
				"surfaces stop reading as faceted plates -- the visible polygons on character faces. "
				"The value is a crease angle in degrees: a corner is smoothed only if its own face "
				"lies within that angle of the average, so 60 rounds off a face while leaving a 90 "
				"degree building corner sharp. 0 keeps flat normals. The PS2 sends no normals of its "
				"own, so nothing in the Remix developer menu can substitute for this.",
				false},
			{"UNTEXZ", "Geometry and Filtering", "Submit untextured draws", knob_type::Boolean, 1, 0, 1, 1,
				nullptr,
				"Routes untextured draws through the Z-to-w calibration and submits them. They are "
				"the majority of a frame in some games.",
				false},
			{"VCOLOR", "Geometry and Filtering", "Use vertex colour", knob_type::Boolean, 1, 0, 1, 1,
				nullptr, "Passes the PS2 per-vertex colour through to Remix.", false},
			{"VCBAKED", "Geometry and Filtering", "Vertex colour is baked lighting", knob_type::Boolean, 0, 0, 1, 1,
				nullptr,
				"Tells Remix the vertex colour is baked lighting rather than albedo, so it is not "
				"multiplied into the material twice.",
				false},
			{"CUTOUT", "Geometry and Filtering", "Alpha-tested draws are cutouts", knob_type::Boolean, 1, 0, 1, 1,
				nullptr, "Classifies alpha-tested draws as cutouts so foliage resolves correctly.", false},
			{"ALPHASTATE", "Geometry and Filtering", "Alpha state handling", knob_type::Integer, 2, 0, 3, 1,
				nullptr, "How much of the PS2 alpha/blend state is forwarded to Remix.", false},
			{"WFLAT", "Geometry and Filtering", "Flat-w rejection", knob_type::Float, 0.001, 0.0, 1.0, 0.001,
				nullptr,
				"Rejects a draw whose w values are this close together, which is how a 2D overlay "
				"looks. Raise to drop more 2D, lower to keep more.",
				false},
			{"MINW", "Geometry and Filtering", "Minimum w", knob_type::Float, 0.01, 0.0, 1000.0, 0.01,
				nullptr, "Draws with a smaller maximum w are skipped.", false},
			{"MAXW", "Geometry and Filtering", "Maximum w", knob_type::Float, 0.0, 0.0, 100000.0, 1.0,
				nullptr, "Draws beyond this w are skipped. 0 disables the gate.", false},
			{"EXPLODEK", "Geometry and Filtering", "Vertex explosion limit", knob_type::Float, 32.0, 0.0, 100000.0, 4.0,
				nullptr,
				"Rejects a draw whose screen extent exceeds this multiple of its w, the signature of "
				"a vertex explosion. 0 disables it.",
				false},
			{"FSTZ", "Geometry and Filtering", "FST Z recovery", knob_type::Boolean, 1, 0, 1, 1, nullptr,
				"Recovers depth for draws that use FST texture coordinates.", false},
			{"FSTFLAT", "Geometry and Filtering", "Accept flat-Z FST draws", knob_type::Boolean, 0, 0, 1, 1,
				nullptr, "Allows FST recovery on draws whose Z barely varies.", false},

			// ------------------------------------------------------------------ Mesh Identity
			{"STABLEID", "Mesh Identity", "Stable mesh identity", knob_type::Boolean, 0, 0, 1, 1, nullptr,
				"Matches this frame's meshes against the previous frame by rigid registration "
				"instead of hashing positions.",
				false},
			{"IDCOLOR", "Mesh Identity", "Include vertex colour in identity", knob_type::Boolean, 1, 0, 1, 1,
				nullptr, "Folds vertex colour into the mesh hash.", true},
			{"IDSLOTS", "Mesh Identity", "Identity slots", knob_type::Integer, 4, 1, 64, 1, nullptr,
				"Candidate slots considered when matching a mesh to a previous one.", false},
			{"MESHCAP", "Mesh Identity", "Mesh cache size", knob_type::Integer, 4096, 16, 65536, 256, nullptr,
				"Maximum live Remix meshes before the least recently used are released.", false},
			{"MESHIDLE", "Mesh Identity", "Mesh idle frames", knob_type::Integer, 120, 1, 100000, 30, nullptr,
				"Frames a mesh may go unused before it is eligible for release.", false},
			{"MESHBUDGET", "Mesh Identity", "Mesh creations per frame", knob_type::Integer, 0, 0, 100000, 32,
				nullptr, "Cap on new meshes per frame. 0 is unlimited.", false},
			{"INSTBUDGET", "Mesh Identity", "Instances per frame", knob_type::Integer, 0, 0, 1000000, 64,
				nullptr, "Cap on instances submitted per frame. 0 is unlimited.", false},
			{"REUSEHANDLE", "Mesh Identity", "Reuse mesh handles", knob_type::Boolean, 0, 0, 1, 1, nullptr,
				"Reuses a mesh handle when the geometry is judged unchanged.", true},
			{"REUSEPOOL", "Mesh Identity", "Mesh reuse pool", knob_type::Integer, 0, 0, 65536, 64, nullptr,
				"Size of the pool of handles held for reuse. 0 disables pooling.", false},
			{"BATCH", "Mesh Identity", "Batch draws", knob_type::Boolean, 0, 0, 1, 1, nullptr,
				"Merges compatible draws into one mesh before submitting.", false},
			{"BATCHRETAIN", "Mesh Identity", "Batch retention", knob_type::Integer, 3, 0, 1000, 1, nullptr,
				"Frames a batch group is kept before being rebuilt.", false},

			// -------------------------------------------------------------------- Diagnostics
			{"DRAWDUMP", "Diagnostics", "Dump per-draw state (frames)", knob_type::Integer, 0, 0, 100000, 10,
				nullptr, "Writes remix_draws.txt describing every submitted draw for N frames.", false},
			{"TEXDUMP", "Diagnostics", "Log decoded textures", knob_type::Boolean, 1, 0, 1, 1, nullptr,
				"Logs each decoded texture's hash and mean colour.", true},
			{"FBMSKDUMP", "Diagnostics", "Log framebuffer-mask draws", knob_type::Boolean, 0, 0, 1, 1,
				nullptr, "Logs draws rejected by the FBMSK gate.", false},
			{"ALBEDOPROBE", "Diagnostics", "Magenta albedo probe", knob_type::Boolean, 0, 0, 1, 1, nullptr,
				"Fills every uploaded texture with magenta. Anything still white is not sampling our "
				"texture at all.",
				true},
			{"STATSFRAMES", "Diagnostics", "Counter interval (frames)", knob_type::Integer, 300, 1, 100000, 60,
				nullptr, "How often the counter block is written to the log.", false},
			{"SCANKICKS", "Diagnostics", "VU kick scan depth", knob_type::Integer, 16, 0, 4096, 8, nullptr,
				"How many VU1 kicks are scanned per frame when recovering the camera.", false},
			{"NODEBUGSCENE", "Diagnostics", "Disable debug scene", knob_type::Boolean, 0, 0, 1, 1, nullptr,
				"Suppresses the built-in debug geometry.", true},
			{"NODRAWINSTANCE", "Diagnostics", "Do not submit instances", knob_type::Boolean, 0, 0, 1, 1,
				nullptr, "Builds meshes but submits no instances. Isolates cost between the two.", true},
			{"SPIKE", "Diagnostics", "Log frame spikes", knob_type::Boolean, 0, 0, 1, 1, nullptr,
				"Logs frames that took unusually long.", true},
		};
	} // namespace

	const knob* knobs(size_t& count)
	{
		count = std::size(s_knobs);
		return s_knobs;
	}
} // namespace remix_ps2
