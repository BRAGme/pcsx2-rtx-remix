// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Remix/RemixRuntime.h"
#include "GS/Renderers/HW/GSTextureCache.h"

#include <string>

// The PS2 -> Remix material bridge.
//
// Identity is GSTextureCache::HashCacheKey (GSTextureCache.h:102-121): a hash of the actual
// swizzled bytes in GS local memory plus the CLUT, so it is stable across runs by construction
// and is the *same* identity PCSX2's own texture-replacement system uses. It is folded to 64
// bits, handed to remixapi_CreateTexture as the texture hash, and then referenced from the
// material through the Remix Plus fork's pseudo-path ("0x%016llX"). That pseudo-path is what
// puts an API-created texture into the same hash namespace as a natively-hashed D3D9 one --
// i.e. it is what makes the value typeable into rtx.conf and taggable in the runtime UI. On
// stock (non-fork) Remix the pseudo-path does not resolve and no modding surface exists, which
// is why everything here is gated on runtime::fork_features().
//
// Threading: GS thread only, called from inside GSRendererHW::DrawPrims and from the frame
// boundary. It reads GSLocalMemory through exactly the route GSTextureReplacements::DumpTexture
// uses, on the same thread the texture cache itself runs on.
namespace remix_ps2
{
	namespace materials
	{
		struct binding
		{
			// Null when no material could be resolved: no source, a render-target source, the
			// fork is unavailable, the per-frame create budget ran out, or decode failed.
			remixapi_MaterialHandle material = nullptr;

			// The value a modder types into rtx.conf. Zero when 'material' is null. It MUST be
			// folded into the caller's mesh hash: Remix binds the material at CreateMesh time,
			// so two draws sharing geometry but not their texture must not share a mesh handle.
			u64 content_hash = 0;
		};

		// CPU-side decoded pixels for a resolved material, for the 2D overlay rasteriser.
		// The payload is the same BGRA8 block handed to CreateTexture and kept resident for the
		// life of the entry, so the pointer is valid until that material is reaped.
		// Returns false when the hash is unknown or carries no pixels.
		bool cpu_pixels(u64 content_hash, const u8*& out_pixels, u32& out_width, u32& out_height);

		// Resets the per-frame CreateTexture budget. Call once per frame, before the reaps.
		void begin_frame();

		// 'source' is the GSTextureCache::Source the draw sampled, or null for an untextured
		// draw. Never throws and never asserts: any failure degrades to a null material.
		binding bind(const runtime& rt, const GSTextureCache::Source* source, u64 frame,
			bool allow_render_target_snapshot);

		// The content hash for a source, computed and nothing else -- no texture upload, no
		// material, no budget consumed, no cache entry.
		//
		// For draws that are REJECTED before bind() but whose texture still matters. Rainbow Six 3's
		// lightmap is the case: it arrives on masked multi-pass draws that the FBMSK gate drops, so
		// the texture never becomes a Remix material and its hash -- the value a modder types into
		// rtx.conf, and the key the emissive and category lists match on -- was never computed at
		// all. Returns 0 when there is no hashable source.
		u64 hash_only(const GSTextureCache::Source* source);

		// One shared white material for untextured draws.
		//
		// Since untextured draws started being submitted rather than dropped (PCSX2_REMIX_UNTEXZ),
		// they are the *majority* of a SOCOM mission frame -- 811,006 of 1,008,379 submitted -- and
		// bind() hands them the null binding because there is no source. Measured result: the frame
		// renders geometrically complete and completely colourless, mean saturation 0.038 with
		// coloured_px 0.00%.
		//
		// PS2 untextured geometry is shaded by per-vertex colour, and that colour is already being
		// written into every remixapi_HardcodedVertex. It just has no material to modulate. This
		// supplies one: no albedo texture, albedoConstant white, so the vertex colour comes through.
		// Created once and cached for the session; it holds no texture, so it is outside the
		// CreateTexture budget and the LRU entirely.
		binding bind_untextured(const runtime& rt);

		// LRU release. Must run *after* the mesh reap: a live mesh holds its material handle,
		// so meshes have to be released before the materials they reference.
		void reap(const runtime& rt, u64 frame);
		void destroy_all(const runtime& rt);

		// Instance category flags the user has tagged this texture with in the Remix developer
		// menu (and saved), read from the runtime's own rtx.conf / user.conf hash lists.
		//
		// This has to be done here, and it is not a duplication of runtime behaviour we could
		// have left alone. dxvk-remix applies those lists in
		// DrawCallState::setupCategoriesForTexture() (rtx_types.cpp:348), whose *only* caller is
		// the native D3D9 draw path (d3d9_rtx.cpp:1064). The Remix API instance path takes its
		// categories exclusively from remixapi_InstanceInfo::categoryFlags
		// (rtx_remix_api.cpp:803, :867). So without this, every tag the user makes -- sky,
		// ignore, decal, particle -- is written to disk and then silently has no effect on
		// anything we submit.
		remixapi_InstanceCategoryFlags categories_for(u64 content_hash);

		// The OR of every category flag currently in the tag table, so a caller can test whether
		// ANY texture carries a category before it computes a content hash to look one up with.
		//
		// The distinction matters because the hash is not free: hash_only() runs the same
		// HashTextureLevel unswizzle-and-hash that makes bind() cost 1.7-2.6 us per call. A draw
		// classifier that wants to consult the tag table BEFORE bind() runs (the sky path does --
		// classification chooses the solver, and the solver is picked before any material exists)
		// would otherwise pay that on every textured draw of every title, tagged or not.
		//
		// Rebuilt with the table, so it moves with generation(). Zero means no tags at all.
		remixapi_InstanceCategoryFlags tagged_category_mask();

		// Re-reads the conf layers if any of them changed on disk. Cheap (a stat per file, at
		// most once a second); call once per frame.
		void refresh_categories();

		// Per-game Remix configuration. Call once per frame beside refresh_categories.
		//
		// PCSX2 is one executable for hundreds of games and Remix keys its user settings by exe
		// identity, so every PS2 title otherwise shares one config. This reads
		// "<exe dir>\<SERIAL>.conf" then "<SERIAL>_<CRC>.conf" -- mirroring PCSX2's own
		// gamesettings\<SERIAL>_<CRC>.ini naming -- and pushes each entry through
		// remixapi_SetConfigVariable, which writes Remix's *user* layer and therefore outranks
		// rtx.conf and any Logic-graph layer. Keys spelled PCSX2_REMIX_* set our own toggles
		// instead, and an existing environment variable always beats the file so an A/B harness
		// cannot be silently overridden.
		//
		// Texture category and emissive hash lists in the same files are handled by
		// refresh_categories, which already reads them (conf_paths appends the per-game layers)
		// with the digit-group-comma parser they need.
		void refresh_game_config(const runtime& rt);

		// Forces the next refresh_game_config to re-apply instead of waiting out its poll
		// interval. Call when the running game may have changed under us -- a save-state load
		// can switch title, and up to a second of frames would otherwise run the previous
		// game's settings.
		void invalidate_game_config();
		void on_state_loaded(const runtime& rt);

		// Bumped every time the tag lists change on disk. The caller MUST fold this into its
		// mesh hash: Remix binds the material into the mesh at CreateMesh time, so a mesh built
		// before a tag changed would keep the pre-tag material for as long as it stays cached.
		u64 generation();

		// One line for the periodic counter block.
		std::string stats_line();

		// Number of unique content hashes seen, for the CLUT-animation measurement.
		u64 unique_hashes();
	} // namespace materials
} // namespace remix_ps2
