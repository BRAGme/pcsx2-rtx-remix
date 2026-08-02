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

		// Resets the per-frame CreateTexture budget. Call once per frame, before the reaps.
		void begin_frame();

		// 'source' is the GSTextureCache::Source the draw sampled, or null for an untextured
		// draw. Never throws and never asserts: any failure degrades to a null material.
		binding bind(const runtime& rt, const GSTextureCache::Source* source, u64 frame);

		// LRU release. Must run *after* the mesh reap: a live mesh holds its material handle,
		// so meshes have to be released before the materials they reference.
		void reap(const runtime& rt, u64 frame);
		void destroy_all(const runtime& rt);

		// One line for the periodic counter block.
		std::string stats_line();

		// Number of unique content hashes seen, for the CLUT-animation measurement.
		u64 unique_hashes();
	} // namespace materials
} // namespace remix_ps2
