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
		// the VIF1 TOPS double-buffer base, 4 = title-specific fixed VU block,
		// 6 = back-slice whose base was recovered through an auto-increment chain (LQI/LQD),
		// 7 = a matrix living in the VF register file, read from vuRegs[1].VF.
		// (5 is the GS-side synthetic probe candidate and never appears here.)
		// Anything but 0 is deterministic: the microcode itself said where the matrix is, so it
		// does not compete on shape score.
		u8 source;

		// What the microprogram does with this matrix's result, straight out of the back-slice.
		// bit 0 = the result feeds a DIV, i.e. it produced the perspective divide's denominator,
		//         which is the definition of a projection.
		// bit 1 = the result is CLIPped, i.e. it is clip space.
		// Zero for anything the slice did not produce.
		u8 flags;
	};

	inline constexpr u8 candidate_flag_feeds_div = 1u << 0;
	inline constexpr u8 candidate_flag_feeds_clip = 1u << 1;

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

		// --- the ucode back-slice's program cache -------------------------------------------
		//
		// The cache is keyed on (ucode hash, start_pc), fills first-come-first-served and NEVER
		// rotates. When it is full, program_for() returns nullptr and the ENTIRE deterministic
		// path is skipped for that entry point for the rest of the session -- the back-slice, the
		// register-resident source 7, and the title-specific fixed block alike. It said nothing
		// when it did this, which is how SOCOM: Combined Assault's mission program lost the one
		// entry point (start_pc 0x2040) holding its register-resident vf01..vf04 chains: the same
		// ucode is entered at more than eight PCs, the first eight to be kicked won the slots, and
		// which eight those are depends on where the session resumed. These make it visible.
		u32 programs_used; // cache slots occupied
		u32 programs_limit; // PCSX2_REMIX_MAXPROGRAMS in force
		u32 programs_refused; // lookups refused this frame because the cache was full
		u32 refused_start_pc; // the most recent refused entry point, so it can be named
		u64 refused_ucode;

		// One contiguous 64-qword neighbourhood around the first deterministic matrix
		// read this frame.  The SOCOM CA back-slice currently resolves an object MVP at
		// VI05+7..10; its neighbouring constants are needed to identify the shared camera
		// factor rather than promote that object matrix as the camera.
		float transform_probe[64 * 4];
		u32 transform_probe_base; // qword index of transform_probe[0]
		u32 transform_probe_matrix; // qword index of the back-sliced matrix row 0
		u64 transform_probe_ucode;
		bool transform_probe_valid;
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
	// SOCOM Combined Assault's GS thread can trail VU1 by roughly 28,000 kicks, so 4,096 slots
	// overwrite its draw's camera before the GS packet arrives. 65,536 retains that observed lag.
	inline constexpr u32 kick_ring_size = 65536;

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
