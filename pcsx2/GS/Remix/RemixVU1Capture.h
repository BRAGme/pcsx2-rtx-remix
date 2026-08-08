// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

#include <atomic>

// The VU1 half of the world-anchored camera.
//
// The GS thread must never read VU1 state. It runs up to VsyncQueueSize (default 2) frames
// behind the EE, and under MTVU a third thread writes vuRegs[1] with no lock anywhere in
// pcsx2\x86\microVU*. The one instant at which VU1 data memory, the live microprogram and
// the GIF packet about to be drawn are simultaneously consistent is inside the XGKICK
// transfer, on whichever thread is executing VU1: VUops.cpp (interpreter) and
// microVU_Lower.inl (recompiler). This module is called from exactly those points, scans
// VU1 data memory for perspective-shaped 4x4 matrices, and publishes the frame's best
// candidates to the GS thread through a seqlock. The GS thread latches once per VSync.
//
// microVU.h is deliberately not involved: it defines microVU0/microVU1 as objects and
// #includes 13 .inl files (it compiles as one translation unit), and prog.cur->data is not
// a verbatim copy anyway because doWholeProgCompare is false. The reachable substitutes are
// vuRegs[1].Mem, vuRegs[1].Micro and vuRegs[1].start_pc, all from VU.h.
//
// The header is dependency-light on purpose -- it is included by core VU translation units,
// so it pulls neither remix_c.h nor <windows.h>. Armed() is one relaxed atomic load, so a
// non-Remix run pays one predicted branch per kick and nothing else.
namespace RemixVU1Capture
{
	// Candidates kept per frame. The GS side re-scores every one of them against the real
	// viewport constants, which the VU side cannot know, so keeping a set rather than a
	// single winner is what stops a shadow, cube-face or UI matrix from costing the frame
	// its camera.
	//
	// Measured on Rainbow Six 3: ~4% of the 1021 scanned windows clear the shape prefilter,
	// so the cut is real and the set has to be wide enough that the true camera survives it.
	inline constexpr u32 max_candidates = 32;

	struct Candidate
	{
		// The 16 floats exactly as they sit in VU1 data memory, ascending from mem_offset.
		// Whether the guest stored the matrix row- or column-major is resolved GS-side,
		// which tries both.
		float m[16];
		// Viewport-independent shape score. Only used to rank candidates for the cut; the
		// real accept/reject is the GS-side split + score_perspective.
		float score;
		u32 mem_offset; // byte offset into vuRegs[1].Mem (of row 0, for a back-sliced matrix)
		u32 start_pc; // vuRegs[1].start_pc at the kick that produced it
		u64 ucode_hash; // FNV-1a over vuRegs[1].Micro -- the ucode fingerprint

		// 0 = heuristic scan, 1 = ucode back-slice (live VI base), 2 = back-slice against
		// the VIF1 TOPS double-buffer base. Anything but 0 is deterministic: the microcode
		// itself said where the matrix is, so it does not compete on shape score.
		u8 source;
	};

	struct Frame
	{
		// Value of g_kick_seq at the instant this frame was published. Together with a live
		// KickSeq() read on the GS thread it measures how far behind the GS side is running,
		// which is what decides whether a draw can be joined to the kick that produced it.
		u64 kick_seq_end;
		u32 count; // candidates in items[]
		u32 kicks_seen;
		u32 kicks_scanned;
		u32 windows_examined;
		u32 windows_survived; // passed the shape prefilter
		u32 kicks_reentrant; // kicks dropped because another thread was already scanning
		u32 sliced_matrices; // matrices the ucode back-slice located this frame
		u32 sliced_published; // of those, how many were read out and published
		Candidate items[max_candidates];
	};

	// Armed only while the Remix renderer is open. Written from the GS thread before any
	// draw can run, read on the VU thread once per XGKICK.
	extern std::atomic<bool> g_armed;

	inline bool Armed() { return g_armed.load(std::memory_order_relaxed); }

	// Monotonic count of XGKICKs this session, incremented on every kick including the ones
	// dropped for reentrancy -- it is a sequence number, not a workload counter, so a gap in it
	// must mean "a kick happened here that we did not scan" rather than nothing at all.
	//
	// Diagnostic only. Read from the GS thread to answer whether draws can be attributed to the
	// kick that produced them; nothing in the render path depends on it.
	extern std::atomic<u64> g_kick_seq;

	inline u64 KickSeq() { return g_kick_seq.load(std::memory_order_relaxed); }

	// The transported twin of the above: the kick sequence carried on the MTGS ring alongside the
	// packet the GS thread is currently transferring. Written by the ring dispatch on seeing a
	// Command::RemixKickSeq, read at draw time.
	//
	// Plain, not atomic, and deliberately so: both the write and the read happen on the GS thread.
	// A live KickSeq() read at draw time is up to two frames stale (measured: 2106 kicks on
	// Rainbow Six 3) because the GS thread trails the EE by VsyncQueueSize -- this is the value
	// that is actually contemporaneous with the draw.
	extern u64 g_gs_kick_seq;

	inline void SetGSKickSeq(u64 seq) { g_gs_kick_seq = seq; }
	inline u64 GSKickSeq() { return g_gs_kick_seq; }

	// Per-kick camera ring. The transported sequence above names the kick a draw was built under;
	// this is where the camera that was live at that kick is kept, so the draw can be placed with
	// its own camera rather than the one frame-latched at VSync.
	//
	// Depth follows the measured lag: a draw arrives up to 2106 kicks after its packet was queued
	// -- two frames at ~1053 kicks/frame on Rainbow Six 3 -- so a shorter ring would have the entry
	// overwritten before the draw asks for it. 4096 is the next power of two, ~360 KB.
	inline constexpr u32 kick_ring_size = 4096;

	// GS thread. Fills `m` with the camera live at `seq`, or at the most recent earlier kick that
	// had one, and reports the VU1 offset it was read from.
	//
	// False means the ring holds nothing usable for that kick. The caller must fall back to the
	// frame camera on false -- never guess, because a wrong camera places geometry in view space,
	// which is the defect this whole mechanism exists to remove.
	bool LookupKickCamera(u64 seq, float (&m)[16], u32& offset);

	void SetArmed(bool enabled);

	// GS thread. Records the VU1 data-memory offset a camera was last accepted from, so the
	// window at that address is always carried into the candidate set no matter how many
	// other windows out-score it that frame.
	//
	// This is what turns an intermittent hit into a lock. The guest re-uploads its camera
	// block to the same VIF unpack destination every frame, so the address is stable even
	// though the contents are not, and ~4% of all scanned windows clear the shape prefilter
	// -- far too many for a fixed-size top-N to be relied on to keep the right one.
	// 0xFFFFFFFF clears the pin.
	void SetPinnedOffset(u32 offset);

	// VU thread, inside the XGKICK transfer. Only call when Armed(); scans within the
	// per-frame budget (PCSX2_REMIX_SCANKICKS, default 16) and publishes.
	void OnXGKick();

	// GS thread. Discards everything published so far: the candidates describe a scene that no
	// longer exists (a save-state load replaces GS local memory and the guest's whole world in
	// one step). Latch() then reports "nothing" until the VU side publishes afresh.
	void DropPublished();

	// GS thread, once per VSync. Copies the published set and starts a new VU-side frame.
	// False means nothing has ever been published.
	bool Latch(Frame& out);
} // namespace RemixVU1Capture
