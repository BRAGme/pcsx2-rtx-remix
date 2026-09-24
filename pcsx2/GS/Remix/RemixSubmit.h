// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

struct WindowInfo;
class GSRendererHW;
namespace remix_ps2::socom { struct LightingRig; struct MissionSnapshot; }

// The RTX Remix API backend's orchestrator.
//
// This module is a tee, not a GSDevice: the stock GSRendererHW and a real GSDevice keep
// servicing every GS-memory semantic, and each committed draw is additionally handed here
// with its vertex/index/context state still pristine. Everything is a no-op unless the Remix
// renderer is selected (or the spike env var is set), so the call sites cost one predicted
// branch.
//
// Threading: entry points run on the GS thread, except the atomic movie
// publications from the EE/IPU thread. OnDrawPrims runs from
// GSRendererHW::DrawPrims; OnVSync/OnGSClose run from the frame boundary.
namespace RemixSubmit
{
	// Native mission state, copied on the CPU thread and delivered in MTGS frame order.
	void OnSocomLighting(const remix_ps2::socom::LightingRig& rig);
	void OnSocomMission(const remix_ps2::socom::MissionSnapshot& mission);
	// PCSX2_REMIX_SPIKE: 0 = off (normal renderer-keyed operation),
	//                    1 = two presenters (stock swapchain stays live),
	//                    2 = sole presenter (D3D11 forced surfaceless).
	// Step 3 of the bring-up plan; scaffolding, removed once the presenter model is settled.
	int SpikeMode();

	// True when the Remix module wants the window for itself. Set from OpenGSDevice when the
	// renderer being opened is Remix, before the device is created -- GSCurrentRenderer is not
	// usable there, it is only assigned later in OpenGSRenderer.
	void SetRendererIsRemix(bool enabled);
	bool RendererIsRemix();

	// Hot-path fast-out for the per-draw tee: one plain bool read, no guarded static init.
	// Written on the GS thread from SetRendererIsRemix before any draw can run.
	extern bool g_armed;
	inline bool Armed() { return g_armed; }

	// Called from GSDevice::AcquireWindow with the window info the host just handed out,
	// before the device gets to look at it. Stashes the real HWND for Remix and, when Remix
	// is to be the sole presenter, rewrites 'wi' to Surfaceless so D3D11 creates no swapchain.
	void OnAcquireWindow(WindowInfo& wi);

	// The tee. Called from the top of GSRendererHW::DrawPrims, which is the one point where
	// the draw is committed AND m_vertex->buff still carries unmutilated Q (Lines2Sprites and
	// the accurate_stq triangle path both destroy it later, inside SetupIA).
	//
	// 'rt_unscaled_width/height' come from the draw's render target (Target::GetUnscaledSize);
	// the Target pointer is type-erased below so this header stays free of
	// GSTextureCache.h, which GSDevice.cpp and GS.cpp would otherwise have to pull in.
	// Zero means "no colour target for this draw" and the draw is skipped.
	//
	// 'tex_source' is the draw's GSTextureCache::Source*, type-erased for the same reason: it
	// is the material bridge's input (TEX0/TEXA/region/lod, from which HashCacheKey::Create
	// recomputes the content hash). Null for an untextured draw. C++ cannot forward-declare a
	// nested class, so a void* is the only way to keep GSTextureCache.h out of this header.
	// rt_target is the current Target*, used for frame-correct goggle readback at the next HUD draw.
	// Observe the exact loading display copy before hardware draw optimization.
	void OnDrawLoadingFrame(const GSRendererHW& renderer, const void* vertices, u32 vertex_count,
		const void* indices, u32 index_count);

	void OnDrawPrims(const GSRendererHW& renderer, int rt_unscaled_width, int rt_unscaled_height,
		const void* tex_source, const void* rt_target);

	// EE/IPU-thread movie publications; no renderer or GS state is read here.
	void OnGuestMovieDecode();
	void OnGuestMovieState(bool active);

	// Hardware GS reset invalidates movie and goggle frame ownership.
	void OnGSReset();

	// Frame boundary, from GSRenderer::VSync after Merge() and before the present block.
	// Null on a blank/failed merge; crop is the renderer's exact visible merged source rectangle.
	void OnVSync(const void* merged_texture, int crop_left, int crop_top, int crop_right, int crop_bottom);

	// A save state was loaded: GS local memory, and with it the guest's entire world, has just
	// been replaced in a single step. Called from the FreezeAction::Load branch of GSfreeze
	// after Defrost succeeds -- which runs on the GS thread, dispatched from the MTGS command
	// loop (MTGS.cpp:527), the same thread as every other entry point here.
	//
	// Everything derived from the old scene has to go: the world camera solved from the old
	// state would un-project the new state's vertices into nonsense, the mesh cache is keyed on
	// hashes nothing will ever match again, and the VU1 seqlock's published candidates were
	// scanned out of the old VU1 memory.
	void OnGSStateLoaded();

	// Renderer teardown, from GSclose. Destroys meshes and lights; the runtime itself stays
	// loaded for the life of the process (re-Startup support in dxvk-remix is unproven).
	void OnGSClose();
} // namespace RemixSubmit
