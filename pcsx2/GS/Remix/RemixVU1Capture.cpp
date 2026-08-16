// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Remix/RemixVU1Capture.h"

// RemixRuntime.h first: it pulls RedtapeWindows.h (NOMINMAX) ahead of remix_c.h's raw
// <windows.h>, which would otherwise macro-poison min/max for everything after it.
#include "GS/Remix/RemixRuntime.h"
#include "GS/Remix/RemixVU1Slice.h"

#include "VU.h"
#include "VUmicro.h"

#include "Config.h"

#include "common/FileSystem.h"
#include "common/Path.h"

#include "fmt/format.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace RemixVU1Capture
{
	namespace
	{
		constexpr u32 s_qword = 16;
		constexpr u32 s_matrix_bytes = 64;

		constexpr u64 s_fnv_seed = 0xCBF29CE484222325ULL;
		constexpr u64 s_fnv_prime = 0x100000001B3ULL;

		// Positions beyond this are not matrix coefficients, they are reinterpreted pointers
		// or packed integer data that happens to sit in the scan window.
		constexpr float s_max_coefficient = 1.0e9f;

		// Per-frame scan budget. A frame issues hundreds of kicks and the camera matrix stays
		// resident in VU1 memory across all of them, so scanning the first few is enough and
		// keeps the cost off the EE/MTVU thread's critical path.
		//
		// Read once from SetArmed on the GS thread, deliberately NOT a function-local static:
		// magic-static initialisation would put a CRT lock and a GetEnvironmentVariableW
		// allocation inside a call made from recompiled VU code, on whichever thread got
		// there first.
		u32 s_scan_budget = 0;

		u32 read_scan_budget()
		{
			const s64 env = remix_ps2::read_env_int(L"PCSX2_REMIX_SCANKICKS", 16);
			if (env <= 0)
				return 0u;

			return static_cast<u32>(std::min<s64>(env, 4096));
		}

		// Whether the exhaustive 16 KB shape scan runs at all, independent of the per-frame kick
		// budget above. These were one setting, which meant turning the scan off also turned off
		// the back-slice that shares the same handler -- so the only stability escape hatch also
		// removed the working camera. Off by default; PCSX2_REMIX_SCANWINDOWS=1 restores it for
		// a title whose microcode does not decode.
		const bool s_scan_windows = remix_ps2::read_env_int(L"PCSX2_REMIX_SCANWINDOWS", 0) > 0;

		// ---- seqlock slot: written by the VU thread, read by the GS thread -----------------
		std::atomic<u32> s_seq{0};

		// Sequence number the GS side must see before it trusts a published frame again.
		// Raised by DropPublished() when the guest's whole scene is replaced.
		std::atomic<u32> s_drop_before_seq{0};

		Frame s_published{};

		// ---- VU-thread private state -------------------------------------------------------
		Frame s_frame{};
		u64 s_frame_hashes[max_candidates]{};
		u32 s_generation_seen = 0;

		// Bumped by the GS thread at every latch; the VU thread starts a new frame when it
		// changes. One relaxed atomic either way -- this is a rate signal, not a barrier.
		std::atomic<u32> s_generation{0};

		// Single-entry guard for the whole scan. Uncontended this is one atomic exchange.
		std::atomic<bool> s_scanning{false};

		constexpr u32 s_no_pin = 0xFFFFFFFFu;
		std::atomic<u32> s_pinned_offset{s_no_pin};

		// Set when PCSX2_REMIX_PINOFFSET named an address by hand. The election calls
		// SetPinnedOffset on EVERY accepted camera (RemixSubmit.cpp), so without this a
		// hand-seeded pin is overwritten by the winner before it is ever read -- which made
		// PINOFFSET useless as an investigative tool exactly when it is most wanted.
		//
		// MEASURED 2026-08-16, why this matters here: SOCOM CA's setup microprogram stores the
		// composed view-projection to VU1 qwords 0..3 (SQ vf01..vf04, 0..3(vi00) at 0x0260..0x0278),
		// and across a full CAMTEST session with 49 distinct matrices at 46 distinct VU1 addresses,
		// offset 0x0000 was scored ZERO times -- the lowest address ever read was 0x0020. The one
		// place the ucode says the camera is written has never been looked at.
		std::atomic<bool> s_pin_user_seeded{false};

		// Re-read PCSX2_REMIX_PINOFFSET from the environment. Called once per frame from
		// publish(), NOT cached and NOT latched: the per-game .conf reaches the environment
		// after this file's arm path has already run, so anything evaluated once at arm time
		// reads the pre-conf value forever. That single mistake has now cost four separate
		// measurements on this project (FRAMETRACE, SLICEVF, UCODEDUMP, and this).
		void refresh_user_pin()
		{
			const s64 env = remix_ps2::read_env_int(L"PCSX2_REMIX_PINOFFSET", -1);
			const bool valid = (env >= 0) && ((env + s_matrix_bytes) <= VU1_MEMSIZE);

			if (valid)
				s_pinned_offset.store(static_cast<u32>(env), std::memory_order_relaxed);
			else if (s_pin_user_seeded.load(std::memory_order_relaxed))
				s_pinned_offset.store(s_no_pin, std::memory_order_relaxed); // knob withdrawn

			s_pin_user_seeded.store(valid, std::memory_order_relaxed);
		}

		struct scan_guard
		{
			~scan_guard() { s_scanning.store(false, std::memory_order_release); }
		};

		// PCSX2_REMIX_SCANTRACE=1: an unbuffered, bounded step trace of the scan, written from
		// whichever thread is executing VU1. The emulator log is buffered and goes through
		// another thread, so it loses the last few lines exactly when they matter -- this is
		// the only thing that survives an abrupt process exit.
		bool s_trace_enabled = false;
		std::FILE* s_trace_file = nullptr;
		u32 s_trace_lines = 0;

		void trace(const char* what, u64 a = 0, u64 b = 0)
		{
			if (!s_trace_enabled || !s_trace_file || s_trace_lines >= 400)
				return;

			++s_trace_lines;
			std::fprintf(s_trace_file, "%s %llu %llu\n", what, static_cast<unsigned long long>(a),
				static_cast<unsigned long long>(b));
			std::fflush(s_trace_file);
		}

		__fi float dot3(const float (&a)[3], const float (&b)[3])
		{
			return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]);
		}

		__fi float length3(const float (&a)[3])
		{
			return std::sqrt(dot3(a, a));
		}

		u64 hash_bits(const float* values, u32 count)
		{
			u64 hash = s_fnv_seed;

			for (u32 i = 0; i < count; ++i)
			{
				u32 bits;
				std::memcpy(&bits, &values[i], sizeof(bits));
				hash = (hash ^ bits) * s_fnv_prime;
			}

			return hash;
		}

		// Fingerprint of the running microprogram. vuRegs[1].Micro is the live, genuinely
		// verbatim copy (microVU's prog.cur->data is not -- doWholeProgCompare is false), and
		// start_pc is the same key mVUsearchProg uses. Recorded for the phase-2 ucode
		// back-slice, which is what a null scan escalates to.
		u64 hash_ucode()
		{
			const u8* const micro = vuRegs[1].Micro;
			if (!micro)
				return 0;

			const u64* words = reinterpret_cast<const u64*>(micro);
			u64 hash = s_fnv_seed;

			for (u32 i = 0; i < (VU1_PROGSIZE / sizeof(u64)); ++i)
				hash = (hash ^ words[i]) * s_fnv_prime;

			return hash;
		}

		// Viewport-independent structural test, which is the only kind the VU thread can run:
		// it has no idea what XYOFFSET or the render target size are.
		//
		// A fused world->screen matrix is M = worldToView * projection * viewport in the
		// row-vector convention. Multiplying that out column by column:
		//
		//   * column 3 of M IS column 2 of worldToView, because the projection contributes
		//     only m[2][3] = 1 and the viewport fold leaves column 3 alone. worldToView's
		//     linear part is a rotation, so the xyz of that column is a UNIT vector -- and
		//     that holds whether or not the guest folded the 12.4 viewport scale in, which is
		//     exactly the thing this side cannot determine. That is the hard prefilter.
		//   * the viewport x/y offsets are determined by the matrix itself:
		//     ox = dot(col0, col3) / |col3|^2 makes u = col0 - ox*col3 equal to the rotation's
		//     column 0 times a scalar, and likewise v from col1, so u and v must come out
		//     mutually orthogonal.
		//   * column 2 is a scalar multiple of column 3 across rows 0..2, because clip z and
		//     clip w both depend on view z alone.
		//
		// The last two are scored, not enforced: a guest that computes z separately, or folds
		// a world scale into the view, still deserves to reach the GS-side splitter. Ranking
		// by this score only decides which candidates make the cut, never whether a frame has
		// a camera.
		bool shape_test(const float* m, bool transposed, float& out_score)
		{
			// M_rowvector[i][j]. If the guest stored the transpose, that entry is m[j*4 + i].
			const auto at = [m, transposed](u32 i, u32 j) -> float {
				return transposed ? m[(j * 4) + i] : m[(i * 4) + j];
			};

			const float c3[3] = {at(0, 3), at(1, 3), at(2, 3)};
			const float len3 = length3(c3);

			// Exactly 1 for a rigid view with a unit-w projection; the band allows a folded
			// world scale or a projection that scales w.
			if (!(len3 > 0.25f) || !(len3 < 4.0f))
				return false;

			const float c0[3] = {at(0, 0), at(1, 0), at(2, 0)};
			const float c1[3] = {at(0, 1), at(1, 1), at(2, 1)};
			const float c2[3] = {at(0, 2), at(1, 2), at(2, 2)};

			const float inv_len_sq = 1.0f / (len3 * len3);
			const float ox = dot3(c0, c3) * inv_len_sq;
			const float oy = dot3(c1, c3) * inv_len_sq;

			const float u[3] = {c0[0] - (ox * c3[0]), c0[1] - (ox * c3[1]), c0[2] - (ox * c3[2])};
			const float v[3] = {c1[0] - (oy * c3[0]), c1[1] - (oy * c3[1]), c1[2] - (oy * c3[2])};

			const float len_u = length3(u);
			const float len_v = length3(v);

			// A degenerate x or y axis is not a camera.
			if (!(len_u > 1e-4f) || !(len_v > 1e-4f))
				return false;

			// Each term is a sharp reciprocal rather than a clamped ramp, and that choice is
			// load-bearing. A real camera satisfies all three identities to float precision,
			// so 1/(1 + k*error) puts it around the theoretical maximum while a window that
			// merely happens to hold three plausible-looking vectors lands an order of
			// magnitude lower. With a saturating ramp every near-miss also scored the maximum,
			// the top-N filled with whichever windows the scan reached first (lowest address),
			// and the true camera -- measured at offset 0x2970 in Rainbow Six 3 -- was crowded
			// out of the set it had already qualified for.
			float score = 2.0f / (1.0f + (100.0f * std::abs(len3 - 1.0f)));

			const float cos_uv = std::abs(dot3(u, v)) / (len_u * len_v);
			score += 2.0f / (1.0f + (100.0f * cos_uv));

			const float len_c2 = length3(c2);
			if (len_c2 > 1e-6f)
			{
				const float cross[3] = {
					(c2[1] * c3[2]) - (c2[2] * c3[1]),
					(c2[2] * c3[0]) - (c2[0] * c3[2]),
					(c2[0] * c3[1]) - (c2[1] * c3[0])};

				const float sin_23 = length3(cross) / (len_c2 * len3);
				score += 1.0f / (1.0f + (100.0f * sin_23));
			}
			else
			{
				// The guest writes GS Z from somewhere else. Neither evidence for nor against.
				score += 0.25f;
			}

			// |u| and |v| are |p00*sx| and |p11*sy| -- an aspect ratio apart, not orders.
			const float ratio = (len_u > len_v) ? (len_u / len_v) : (len_v / len_u);
			score += 1.0f / (1.0f + std::max(0.0f, ratio - 1.0f));

			out_score = score;
			return std::isfinite(score);
		}

		// --- per-kick camera ring -----------------------------------------------------------
		//
		// Written by the VU thread on every kick, read by the GS thread up to two frames later.
		// Per-slot seqlock: the stamp is cleared first, written last, and re-read by the reader
		// after its copy, so a reader that raced a writer reports a miss instead of returning a
		// torn matrix.
		struct KickCameraSlot
		{
			std::atomic<u64> stamp; // kick_seq + 1 once complete; 0 while being written
			float m[16];
			u32 offset;
			u32 valid; // 0 = no readable camera at the pinned offset for this kick
		};

		KickCameraSlot s_kick_ring[kick_ring_size]{};

		// How far back a lookup will walk when the exact kick recorded nothing. One frame's worth:
		// past that, "the camera at this kick" stops being a true statement.
		constexpr u64 s_kick_lookback = 2048;

		// How many of a frame's scanned kicks may contribute the pinned window.
		//
		// 4 was hardcoded, and it is right for a title that writes its camera to the pinned
		// address early in the frame -- R6 3 save state 9 does. Save state 7 does not: its camera
		// reaches VU1[0x240] later, so within the first four kicks the address holds something
		// else and the pinned re-read never offers the real matrix. Measured there: 0x240 is the
		// ONLY offset of 20,000 distinct candidates whose split ever succeeds, and it surfaces
		// twice in 30 seconds.
		//
		// Raising this trades set capacity for reach: the address is re-uploaded per frame but its
		// contents change between kicks, so each distinct content costs one 1000-scored slot out
		// of max_candidates. Default 4 keeps the old behaviour exactly.
		u32 pinned_kick_window()
		{
			static const u32 value = []() -> u32 {
				const s64 env = remix_ps2::read_env_int(L"PCSX2_REMIX_PINKICKS", 4);
				return (env <= 0) ? 0u : static_cast<u32>(std::min<s64>(env, 4096));
			}();

			return value;
		}

		bool finite_window(const float* m);

		// Finite is not the same as usable: an all-zero block passes every finiteness test, and
		// VU1[pin] reads as all zeros for long stretches of every frame because the guest only
		// holds a camera there part of the time.
		bool usable_matrix(const float* m)
		{
			bool any_nonzero = false;
			for (u32 i = 0; i < 16 && !any_nonzero; ++i)
				any_nonzero = (m[i] != 0.f);

			return any_nonzero && finite_window(m);
		}

		// The last contents of the pinned address that looked like a usable matrix, and the
		// generation it was seen in.
		//
		// The scan samples only the first few kicks of a frame, and the camera is not reliably at
		// the pinned address during them -- which is why R6 3 save state 7 hovers around half its
		// frames world-anchored while state 9 sits at 99.8%. Remembering the last good read lets a
		// frame that sampled an empty address still offer a camera instead of dropping the whole
		// frame into view space.
		float s_pin_retained[16]{};
		bool s_pin_retained_valid = false;
		u32 s_pin_retained_generation = 0;

		// Generations (frames) a retained matrix stays offerable. 0 (default) disables retention
		// and restores the previous behaviour exactly. A retained camera is by definition stale, so
		// this trades "the world lags the view by a few frames" against "the world is glued to the
		// view for this frame" -- the second is worse, but only up to a point, hence the bound.
		u32 pin_hold_frames()
		{
			static const u32 value = []() -> u32 {
				const s64 env = remix_ps2::read_env_int(L"PCSX2_REMIX_PINHOLD", 0);
				return (env <= 0) ? 0u : static_cast<u32>(std::min<s64>(env, 600));
			}();

			return value;
		}

		void record_kick_camera(u64 seq)
		{
			KickCameraSlot& slot = s_kick_ring[seq & (kick_ring_size - 1)];

			// Mark in-progress before touching the payload, and fence so the clear cannot be
			// reordered after the writes it is meant to protect.
			slot.stamp.store(0, std::memory_order_relaxed);
			std::atomic_thread_fence(std::memory_order_release);

			const u32 offset = s_pinned_offset.load(std::memory_order_relaxed);
			const u8* const mem = vuRegs[1].Mem;
			u32 valid = 0;

			if (mem && offset != s_no_pin && (offset + s_matrix_bytes) <= VU1_MEMSIZE)
			{
				std::memcpy(slot.m, mem + offset, s_matrix_bytes);

				// A kick with nothing at the pin must record nothing and let the lookup walk back.
				// Accepting an empty address here is what produced a phantom "second camera" that
				// was really sixteen zeros.
				valid = usable_matrix(slot.m) ? 1u : 0u;
			}

			slot.offset = offset;
			slot.valid = valid;

			slot.stamp.store(seq + 1, std::memory_order_release);
		}

		bool finite_window(const float* m)
		{
			for (u32 i = 0; i < 16; ++i)
			{
				if (!std::isfinite(m[i]) || std::abs(m[i]) > s_max_coefficient)
					return false;
			}

			return true;
		}

		// Keeps the frame's top max_candidates by shape score, de-duplicated by content.
		// VU1 memory is double-buffered through VIF1.TOPS, so the same matrix routinely
		// appears at two addresses in the same scan.
		// A tie-break for candidates the microcode located, which all used to enter at a flat 1000.
		//
		// That flat score is worse than a lottery. insert_candidate evicts on
		// `score <= worst -> return`, so once 32 slots hold 1000 every later 1000 is refused
		// outright: the set freezes on the first 32 distinct matrices of the frame and nothing
		// after them can ever get in -- not even the pinned re-read, which also scored 1000. On
		// R6 3 save state 7 the microprogram emits ~20,000 distinct matrices and the real camera
		// arrives late, so it was structurally unable to be considered.
		//
		// Both terms are pure bonuses on top of the existing score, so nothing is demoted relative
		// to today and a title whose camera has neither property ranks exactly as it did.
		//
		// Measured over 24,204 dumped candidates on state 7 (the real camera is 2 of them):
		//   unit forward axis alone   2,772 (11.45%)
		//   z-column == w-column      35     (0.14%)
		//   both                      2      (0.008%)  <- exactly the real camera, twice
		float slice_rank_bonus(const float* m)
		{
			// Rows 0-2 of the w column are the view direction of a view-projection, so a real
			// camera has them unit length. An arbitrary block of VU1 floats rarely does.
			const float wx = m[3];
			const float wy = m[7];
			const float wz = m[11];
			const float mag = std::sqrt((wx * wx) + (wy * wy) + (wz * wz));
			const float unit = std::isfinite(mag) ?
				std::max(0.f, 1.f - std::min(1.f, std::abs(mag - 1.f))) : 0.f;

			// z column identical to the w column, i.e. z = w + const. R6 3 emits exactly this on
			// both measured save states. A title without the pathology scores 0 here and its
			// ranking is unchanged; this can only ever promote, never demote.
			const bool z_is_w = (m[2] == m[3]) && (m[6] == m[7]) && (m[10] == m[11]);

			return unit + (z_is_w ? 1.f : 0.f);
		}

		// Default OFF, on measurement. Three 30 s runs per arm on R6 3 state 7, world-anchored
		// share of frames:
		//
		//   bonus on   37.9  41.0  45.3   (mean 41.4)
		//   bonus off  35.3  48.4  68.3   (mean 50.7)
		//
		// The ranges overlap and three runs cannot settle it, but the bonus is certainly not the
		// improvement it was written to be, and the arm without it is ahead on both mean and
		// median. What actually recovered this state was giving the pinned re-read a score above
		// the slice band (see the 2000 below) so the eviction rule stops refusing it -- that is in
		// both arms, and both are far above the 0 / 0 / 24.9 the original flat 1000 produced.
		//
		// Kept as a knob rather than deleted because the discriminator itself is sound in
		// isolation: over 24,204 dumped candidates it selects 2, and both are the real camera.
		// Something about applying it as an insertion score is what does not carry through.
		bool slice_rank_enabled()
		{
			static const bool value = remix_ps2::read_env_int(L"PCSX2_REMIX_SLICERANK", 0) != 0;
			return value;
		}

		float ranked(float base, const float* m)
		{
			return slice_rank_enabled() ? (base + slice_rank_bonus(m)) : base;
		}

		// Knobs read on the kick path.
		//
		// NOT a `static const` local and NOT a namespace-scope `const`: the renderer goes live
		// before the per-game <SERIAL>.conf is applied, so anything latched at first call -- or,
		// worse, at static-init -- is the pre-conf value for the whole session. The per-game conf
		// also calls SetEnvironmentVariableW directly and bumps no generation counter, so a
		// generation-cached reader never re-reads it either. Both of those have already shipped
		// here as silent no-ops (see s_scan_windows above, which still has the defect).
		//
		// The cost is one GetEnvironmentVariableW per scanned kick -- the scan budget is 16 kicks
		// per frame by default -- so these are read ONCE per kick into locals, never per candidate
		// and never per window.
		bool env_flag_live(const wchar_t* name)
		{
			return remix_ps2::read_env_int(name, 0) != 0;
		}

		void insert_candidate(const float* m, float score, u32 offset, u32 start_pc, u64 ucode,
			u8 source = 0, u8 flags = 0)
		{
			const u64 content = hash_bits(m, 16);

			const u32 live = std::min(s_frame.count, max_candidates);
			for (u32 i = 0; i < live; ++i)
			{
				if (s_frame_hashes[i] == content)
					return;
			}

			u32 slot = live;

			// '>=' not '==': the accumulator is written from whichever thread executes VU1,
			// and under MTVU that is not always the same one. The scan itself is serialised
			// (see the reentrancy guard in OnXGKick), but a count that ever slipped past the
			// array bound here would write straight off the end of items[] into the statics
			// behind it, which is silent corruption rather than an honest failure.
			if (slot >= max_candidates)
			{
				u32 worst = 0;
				for (u32 i = 1; i < max_candidates; ++i)
				{
					if (s_frame.items[i].score < s_frame.items[worst].score)
						worst = i;
				}

				if (score <= s_frame.items[worst].score)
					return;

				slot = worst;
			}
			else
			{
				++s_frame.count;
			}

			Candidate& out = s_frame.items[slot];
			std::memcpy(out.m, m, sizeof(out.m));
			out.score = score;
			out.mem_offset = offset;
			out.start_pc = start_pc;
			out.ucode_hash = ucode;
			out.source = source;
			out.flags = flags;
			s_frame_hashes[slot] = content;
		}

		// --- ucode back-slice, cached per (ucode hash, start_pc) --------------------------
		//
		// This is the primary camera source. The heuristic window scan above stays as the
		// fallback, exactly where it was, because a program whose transform this cannot
		// decode still deserves a chance.
		// THE CEILING THAT LOST THE CAMERA, and it is worth stating exactly because nothing in any
		// log said it was happening.
		//
		// This cache is keyed on (ucode hash, start_pc). One VU1 microprogram has MANY entry
		// points -- SOCOM: Combined Assault's mission ucode 0xd74d4042a48b1ba8 is entered at at
		// least seven distinct PCs -- and each entry point is a separate slice, because Analyze()
		// walks forward from start_pc. The cache filled first-come-first-served at 8, never
		// rotated, and program_for() then returned nullptr for every later entry point FOR THE
		// REST OF THE SESSION. That skips the whole `if (program)` block in OnXGKick: the
		// back-slice, the register-resident source 7, AND the title-specific fixed block.
		//
		// MEASURED on the 2026-08-16 09:32 session log. Six entry points on that one ucode were
		// granted slots (0x0000, 0x16b8, 0x18c8, 0x1978, 0x1988, 0x1a18) -- five of them proven by
		// a `src=socom-fixed` candidate, which is emitted unconditionally inside that block. The
		// seventh, start_pc 0x2040, produced a `src=pinned` row (inserted AFTER and outside the
		// block, so the kick was certainly scanned) and NOT ONE candidate from inside it -- no
		// socom-fixed, no slice, no slice-vf. The only way that happens is program_for() returning
		// nullptr. 0x2040 is the entry point whose register-resident vf01..vf04 chains are the
		// only camera-tracking matrix this project has found; two sessions earlier it happened to
		// arrive within the first eight and published 133 rows.
		//
		// So which entry points get sliced has been decided by a race between the first few kicks
		// of a session, silently, and the camera's discoverability rode on it.
		constexpr u32 s_max_programs_cap = 64;

		struct ProgramEntry
		{
			bool used = false;
			u64 ucode_hash = 0;
			u32 start_pc = 0;
			RemixVU1Slice::Program program;
		};

		ProgramEntry s_programs[s_max_programs_cap];
		u32 s_program_count = 0;

		// PCSX2_REMIX_MAXPROGRAMS -- how many (ucode, start_pc) pairs may be sliced per session.
		//
		// DEFAULT 8, i.e. exactly what shipped, so this build changes nothing on its own. Raising
		// it CHANGES THE PICTURE and the mechanism is named rather than hedged: a newly-sliced
		// entry point contributes candidates to the 32-slot per-frame set, which changes which
		// candidates survive, and any of them can in principle be elected.
		//
		// What bounds that on THIS title, stated as a bound and not as a guarantee: the elected
		// camera is the source-4 fixed block at score 105 (5.00 shape + the 100 source bonus), and
		// every candidate a new entry point can add enters at most 6 + 10 = 16, while sources 6
		// and 7 cannot be elected at all while PCSX2_REMIX_DIVCAM = 0. The 3000/2000-scored fixed
		// and pinned entries are never the eviction victim, because insert_candidate evicts the
		// LOWEST score. On another title, or with SOCOMFIXED = 0 or DIVCAM > 0, none of that
		// holds. It is not invisible; it is bounded, and only here.
		//
		// Read LIVE and only on a cache MISS -- a handful of times per session, never on the hot
		// path -- so a value that arrives with the per-game .conf after the arm still takes
		// effect. This file's own note on env_flag_live records why nothing here may be latched.
		u32 program_cache_limit()
		{
			const s64 value = remix_ps2::read_env_int(L"PCSX2_REMIX_MAXPROGRAMS", 8);
			return static_cast<u32>(std::clamp<s64>(value, 1, s_max_programs_cap));
		}

		void dump_ucode(const ProgramEntry& entry, const u8* micro)
		{
			const std::string& dir = EmuFolders::Logs.empty() ? EmuFolders::AppRoot : EmuFolders::Logs;

			// The raw image is a property of the ucode alone, so it keeps its old name and is
			// simply rewritten with identical bytes by each entry point.
			const std::string bin_path =
				Path::Combine(dir, fmt::format("remix_ucode_{:016x}.bin", entry.ucode_hash));
			if (std::FILE* f = FileSystem::OpenCFile(bin_path.c_str(), "wb"))
			{
				std::fwrite(micro, 1, VU1_PROGSIZE, f);
				std::fclose(f);
			}

			// THE DISASSEMBLY IS NOT. Describe() walks forward from THIS entry's start_pc, so one
			// ucode produces a different listing per entry point -- and the old name keyed on the
			// ucode alone meant all of them wrote to one file and clobbered each other. The
			// survivor was whichever entry point happened to be analysed last, so the listing for
			// the entry point actually under investigation (0x2040 here) was routinely destroyed
			// by an unrelated one. One file per (ucode, start_pc).
			const std::string txt_path = Path::Combine(dir,
				fmt::format("remix_ucode_{:016x}_pc{:04x}.txt", entry.ucode_hash, entry.start_pc));
			if (std::FILE* f = FileSystem::OpenCFile(txt_path.c_str(), "w"))
			{
				const std::string text = RemixVU1Slice::Describe(micro, entry.start_pc, entry.program);
				std::fwrite(text.data(), 1, text.size(), f);
				std::fclose(f);
			}
		}

		// Last value program_cache_limit() returned, so the stats line can report the limit on
		// frames that had no cache miss to read it on. Not the knob's authority -- the miss path
		// below re-reads it every time -- just what to print.
		u32 s_program_limit_seen = 8;

		const RemixVU1Slice::Program* program_for(const u8* micro, u64 ucode_hash, u32 start_pc)
		{
			// Free, and set on every kick so the occupancy reported is this frame's truth rather
			// than only that of frames which happened to add an entry.
			s_frame.programs_used = s_program_count;
			s_frame.programs_limit = s_program_limit_seen;

			for (u32 i = 0; i < s_program_count; ++i)
			{
				if (s_programs[i].ucode_hash == ucode_hash && s_programs[i].start_pc == start_pc)
					return &s_programs[i].program;
			}

			// MISS. Everything below runs at most once per distinct entry point per session, so
			// the two environment reads here cost nothing measurable and both are live.
			const u32 limit = program_cache_limit();
			s_program_limit_seen = limit;
			s_frame.programs_limit = limit;

			if (s_program_count >= limit)
			{
				// Refusals were silent. They are the reason a camera can be present in VU1 and
				// unreachable, so they are counted and the offending identity is carried to the
				// GS side, which prints it on the vu stats line.
				++s_frame.programs_refused;
				s_frame.refused_start_pc = start_pc;
				s_frame.refused_ucode = ucode_hash;
				return nullptr;
			}

			ProgramEntry& entry = s_programs[s_program_count++];
			entry.used = true;
			entry.ucode_hash = ucode_hash;
			entry.start_pc = start_pc;
			RemixVU1Slice::Analyze(micro, start_pc, entry.program);

			s_frame.programs_used = s_program_count;

			// PCSX2_REMIX_UCODEDUMP, read HERE and not latched at arm.
			//
			// It was `s_ucode_dump_enabled`, assigned once in SetArmed(true) from the environment.
			// SetArmed runs with the renderer -- t = 6.39 s on the session that motivated this --
			// and the per-game settings reach the environment after it, t = 6.50 s. So the flag
			// latched the pre-config value and the dump never fired, which is why the newest
			// listing for this title's mission ucode on disk is dated 2026-08-02. That is the same
            // latching defect that has now cost this project six separate measurements. Reading it
			// on the miss path costs one GetEnvironmentVariableW per distinct entry point.
			if (remix_ps2::read_env_int(L"PCSX2_REMIX_UCODEDUMP", 0) != 0)
				dump_ucode(entry, micro);

			return &entry.program;
		}

		// Reads one back-sliced matrix out of VU1 data memory. 'use_tops' selects the TOPS
		// hypothesis; otherwise the live VI register the program itself computed is the base,
		// which already carries whatever double-buffer bank is current.
		//
		// The address of a row is
		//
		//     VI[vi_base] as it reads AT THIS KICK  +  vi_delta  +  imm
		//
		// and vi_delta is what makes an auto-incrementing load readable at all. Before it, an
		// LQI/LQD row was rejected outright, because reading VI at the kick for a program that
		// streams its matrix with LQI lands wherever the stream has since advanced to -- SOCOM
		// Combined Assault's mission program is four qwords past row 0 by the time it kicks. The
		// slicer now counts those four increments out of the instruction stream, so the base is
		// recovered rather than guessed. vi_delta is 0 for the classic LQ+immediate case, which
		// is bit-for-bit the behaviour this function has always had.
		bool read_sliced_matrix(const u8* mem, const RemixVU1Slice::Matrix& matrix,
			bool use_tops, u32 tops, float (&out)[16], u32& out_offset)
		{
			if (matrix.register_rows)
				return false;

			for (u32 row = 0; row < 4; ++row)
			{
				const RemixVU1Slice::RowLoad& load = matrix.rows[row];

				switch (load.kind)
				{
					case RemixVU1Slice::LoadKind::LQ:
						break;
					case RemixVU1Slice::LoadKind::LQI:
					case RemixVU1Slice::LoadKind::LQD:
						// Only readable once the slicer tied the register back to a kick.
						if (load.kick_dir == RemixVU1Slice::KickDir::None)
							return false;
						break;
					default:
						return false;
				}

				const u32 base = use_tops ? tops : static_cast<u32>(vuRegs[1].VI[load.vi_base].US[0]);
				const s32 displacement = static_cast<s32>(load.vi_delta) + static_cast<s32>(load.imm);
				const u32 qword = (base + static_cast<u32>(displacement)) & 0x3FFu;
				const u32 offset = qword * 16u;

				if ((offset + 16u) > VU1_MEMSIZE)
					return false;

				std::memcpy(&out[row * 4], mem + offset, 16);

				if (row == 0)
					out_offset = offset;
			}

			return true;
		}

		// Reads a matrix that never touches VU1 data memory, because its four rows are live VF
		// registers a different microprogram left behind.
		//
		// This is the case the address-based back-slice is structurally blind to, and on SOCOM
		// Combined Assault it is the one that matters: seven separate chains in the mission
		// program transform with vf01..vf04, five of them feeding the perspective divide
		// directly, and not one instruction in the whole 16 KB of microcode loads those four
		// registers. Nothing about this is title-specific -- a chain whose rows have no writer is
		// register-resident by definition, whatever the program.
		bool read_register_matrix(const RemixVU1Slice::Matrix& matrix, float (&out)[16])
		{
			if (!matrix.register_rows)
				return false;

			for (u32 row = 0; row < 4; ++row)
			{
				const u32 reg = matrix.rows[row].vf_reg;
				if (reg >= 32)
					return false;

				// vf00 is the hardwired constant (0, 0, 0, 1). A "matrix" row that is really the
				// constant means the chain was not a 4x4 transform at all.
				if (reg == 0)
					return false;

				std::memcpy(&out[row * 4], vuRegs[1].VF[reg].F, 16);
			}

			return true;
		}

		void capture_transform_probe(const u8* mem, u32 matrix_offset, u64 ucode)
		{
			if (s_frame.transform_probe_valid)
				return;

			constexpr u32 qwords = 64;
			constexpr u32 half = qwords / 2;
			const u32 matrix_qword = matrix_offset / s_qword;
			const u32 base_qword = (matrix_qword + 1024u - half) & 0x3FFu;

			for (u32 i = 0; i < qwords; ++i)
			{
				const u32 qword = (base_qword + i) & 0x3FFu;
				std::memcpy(&s_frame.transform_probe[i * 4], mem + (qword * s_qword), s_qword);
			}

			s_frame.transform_probe_base = base_qword;
			s_frame.transform_probe_matrix = matrix_qword;
			s_frame.transform_probe_ucode = ucode;
			s_frame.transform_probe_valid = true;
		}

		void publish()
		{
			// Pick up a PINOFFSET that arrived with the per-game .conf, which lands after the
			// arm path has already run. Once per frame inside the scan guard -- not per kick,
			// not per candidate.
			refresh_user_pin();

			// Stamped here rather than per kick: publish() runs inside the scan guard, so this is
			// the last point at which s_frame is provably owned by one thread.
			s_frame.kick_seq_end = g_kick_seq.load(std::memory_order_relaxed);

			const u32 seq = s_seq.load(std::memory_order_relaxed);

			s_seq.store(seq + 1, std::memory_order_relaxed);
			std::atomic_thread_fence(std::memory_order_release);

			std::memcpy(&s_published, &s_frame, sizeof(Frame));

			std::atomic_thread_fence(std::memory_order_release);
			s_seq.store(seq + 2, std::memory_order_release);
		}
	} // namespace

	std::atomic<bool> g_armed{false};
	std::atomic<u64> g_kick_seq{0};
	u64 g_gs_kick_seq = 0;

	void SetArmed(bool enabled)
	{
		if (enabled)
		{
			// A fresh session must not inherit the previous one's candidates.
			s_seq.store(0, std::memory_order_relaxed);
			g_kick_seq.store(0, std::memory_order_relaxed);
			g_gs_kick_seq = 0;

			// Clearing the stamps is enough to retire the ring: any payload behind a zero stamp
			// is unreachable through LookupKickCamera.
			for (u32 i = 0; i < kick_ring_size; ++i)
				s_kick_ring[i].stamp.store(0, std::memory_order_relaxed);

			s_pin_retained_valid = false;
			s_pin_retained_generation = 0;
			s_drop_before_seq.store(0, std::memory_order_relaxed);
			s_published = Frame{};
			s_frame = Frame{};
			s_generation.store(0, std::memory_order_relaxed);
			s_generation_seen = 0;
			s_scanning.store(false, std::memory_order_relaxed);
			s_scan_budget = read_scan_budget();

			// The back-slice cache is per session: a new game means new microprograms.
			for (u32 i = 0; i < s_max_programs_cap; ++i)
				s_programs[i] = ProgramEntry{};
			s_program_count = 0;
			s_program_limit_seen = 8;

			// PCSX2_REMIX_UCODEDUMP is NOT read here any more. It used to latch into
			// s_ucode_dump_enabled at this point, which is before the per-game settings reach the
			// environment, so it was the pre-config value for the whole session and the dump
			// silently never fired. It is now read live on the program-cache miss path, which is
			// the only place it is acted on. See program_for().

			const std::wstring env = remix_ps2::read_env(L"PCSX2_REMIX_SCANTRACE");
			s_trace_enabled = !env.empty() && env[0] != L'0';
			if (s_trace_enabled && !s_trace_file)
			{
				const std::string& dir = EmuFolders::Logs.empty() ? EmuFolders::AppRoot : EmuFolders::Logs;
				s_trace_file = FileSystem::OpenCFile(Path::Combine(dir, "remix_scantrace.txt").c_str(), "w");
			}
		}

		if (enabled)
		{
			// PCSX2_REMIX_PINOFFSET seeds the pin by hand, so a title whose camera address is
			// already known does not have to win the shape ranking once before it locks.
			// Seeded here only so a value already in the ENVIRONMENT at launch takes effect on
			// frame one. The authoritative read is refresh_user_pin() below, called per frame,
			// because a per-game .conf is applied AFTER this arm runs -- measured at t=7.08 s
			// against an arm that precedes it. Seeding only here is what made PINOFFSET = 0
			// silently no-op: the seed read -1, and the election then overwrote the pin.
			refresh_user_pin();
		}

		g_armed.store(enabled, std::memory_order_relaxed);
	}

	void SetPinnedOffset(u32 offset)
	{
		// A hand-seeded pin outranks the election's. See s_pin_user_seeded: the election calls
		// this on every accepted camera, so honouring it here would clobber PINOFFSET within a
		// frame of arming and the address under investigation would never be read.
		if (s_pin_user_seeded.load(std::memory_order_relaxed))
			return;

		s_pinned_offset.store(offset, std::memory_order_relaxed);
	}

	bool LookupKickCamera(u64 seq, float (&m)[16], u32& offset)
	{
		for (u64 back = 0; (back <= s_kick_lookback) && (back <= seq); ++back)
		{
			const u64 want = seq - back;
			const KickCameraSlot& slot = s_kick_ring[want & (kick_ring_size - 1)];

			const u64 stamp = slot.stamp.load(std::memory_order_acquire);
			if (stamp != (want + 1))
				continue; // never written, or already recycled by a later kick

			float copy[16];
			std::memcpy(copy, slot.m, sizeof(copy));
			const u32 copy_offset = slot.offset;
			const u32 copy_valid = slot.valid;

			// Re-read the stamp after the copy: if a writer took this slot mid-read the payload
			// above is a mix of two kicks and must be discarded, not returned.
			std::atomic_thread_fence(std::memory_order_acquire);
			if (slot.stamp.load(std::memory_order_relaxed) != stamp)
				continue;

			if (copy_valid == 0)
				continue; // this kick genuinely had no camera; keep walking back

			std::memcpy(m, copy, sizeof(copy));
			offset = copy_offset;
			return true;
		}

		return false;
	}

	void OnXGKick()
	{
		// Before the reentrancy gate on purpose: this is the kick *sequence*, so a kick that is
		// dropped below still has to advance it or the numbering stops describing the guest.
		const u64 seq = g_kick_seq.fetch_add(1, std::memory_order_relaxed) + 1;

		// Also ahead of the gate, and ahead of the scan budget further down. The budget caps the
		// SEARCH for a camera; this is not a search -- it is a 64-byte read from an address the
		// search already found. At ~1053 kicks/frame against a budget of 16, putting it behind the
		// gate would record 1.5% of kicks and defeat the point of a per-kick ring. Two threads
		// cannot collide here: distinct kicks take distinct sequence numbers, so distinct slots.
		record_kick_camera(seq);

		// VU1 is executed by the EE thread, or by the MTVU thread, or by both in the same
		// session (vif1's _vuXGKICKTransfer runs on the EE side while vu1Thread executes the
		// program). Everything below writes one shared per-frame accumulator, so let exactly
		// one thread in at a time and count what was dropped rather than interleaving.
		if (s_scanning.exchange(true, std::memory_order_acquire))
		{
			++s_frame.kicks_reentrant;
			return;
		}

		const scan_guard guard;

		const u32 generation = s_generation.load(std::memory_order_relaxed);
		if (generation != s_generation_seen)
		{
			s_generation_seen = generation;
			s_frame = Frame{};
		}

		++s_frame.kicks_seen;

		if (s_frame.kicks_scanned >= s_scan_budget)
			return;

		const u8* const mem = vuRegs[1].Mem;
		if (!mem)
			return;

		++s_frame.kicks_scanned;

		trace("enter", s_frame.kicks_scanned, reinterpret_cast<u64>(mem));

		const u64 ucode = hash_ucode();
		const u32 start_pc = vuRegs[1].start_pc;

		trace("ucode", ucode, start_pc);

		// --- the deterministic path, first -------------------------------------------------
		// The microcode says where its transform lives, so these go in ahead of anything the
		// shape scan finds. They still go through the GS side's normalise/split/score gate,
		// which is what confirms a decoded address actually holds a camera.
		if (const RemixVU1Slice::Program* program = program_for(vuRegs[1].Micro, ucode, start_pc))
		{
			s_frame.sliced_matrices += program->count;

			// PCSX2_REMIX_SLICEAUTO -- publish matrices whose data-memory base had to be
			// recovered through an auto-increment chain (an LQI/LQD row, or an LQ whose base
			// register moves between the load and the kick). CHANGES THE PICTURE when on: these
			// are extra entries in a 32-slot candidate set, so which of the existing candidates
			// survives the frame can change even before one of them is elected. Default off.
			const bool slice_auto = env_flag_live(L"PCSX2_REMIX_SLICEAUTO");

			// PCSX2_REMIX_SLICEVF -- publish matrices whose four rows are live VF registers.
			// Same warning: extra candidates, so it can change the picture. Default off.
			const bool slice_vf = env_flag_live(L"PCSX2_REMIX_SLICEVF");

			// PCSX2_REMIX_DIVSCORE -- see the use site below. Default off; read live per kick.
			const bool div_score_enabled = env_flag_live(L"PCSX2_REMIX_DIVSCORE");

			// PCSX2_REMIX_SOCOMFIXED -- the hand-picked qword 8..11 block below. Default 1, i.e.
			// exactly what shipped; setting it to 0 removes a candidate that currently enters at
			// 3000 and wins the election outright, so 0 CHANGES THE PICTURE. It exists so the
			// CAMTEST audit can be run without this shortcut drowning the table.
			const bool socom_fixed = remix_ps2::read_env_int(L"PCSX2_REMIX_SOCOMFIXED", 1) != 0;

			// A title-specific fixed address, kept only because removing it is itself a picture
			// change that nothing has yet measured. It is the hand-picked-address hack the
			// back-slice exists to replace; the generic replacements are the two knobs above.
			if (socom_fixed && ucode == 0xd74d4042a48b1ba8ULL)
			{
				constexpr u32 socom_camera_offset = 8u * s_qword;
				float m[16];
				std::memcpy(m, mem + socom_camera_offset, sizeof(m));
				if (finite_window(m))
					insert_candidate(m, ranked(3000.f, m), socom_camera_offset, start_pc, ucode, 4);
			}

			// VU.h's own accessor, so this file never has to know about vif0/vif1 selection.
			// Under MTVU the authoritative copy is vu1Thread.vifRegs; this is the secondary
			// hypothesis only, and the live-VI path above already covers the normal case.
			const u32 tops = static_cast<u32>(vuRegs[1].GetVifRegs().tops);

			for (u32 i = 0; i < program->count; ++i)
			{
				const RemixVU1Slice::Matrix& matrix = program->items[i];

				u8 flags = 0;
				if (matrix.feeds_div)
					flags |= candidate_flag_feeds_div;
				if (matrix.feeds_clip)
					flags |= candidate_flag_feeds_clip;

				float m[16];
				u32 offset = 0;

				if (matrix.register_rows)
				{
					// Only the ones the microprogram actually divides or clips by. A chain whose
					// rows happen to be registers is not evidence of anything; a chain whose
					// result becomes the perspective divide's denominator is a projection by
					// definition, and that is the whole discriminator.
					if (!slice_vf || !(matrix.feeds_div || matrix.feeds_clip))
						continue;

					if (read_register_matrix(matrix, m) && finite_window(m))
					{
						// PCSX2_REMIX_DIVSCORE -- lift a register-resident matrix the MICROCODE
						// ITSELF marked as feeding the perspective divide above an undistinguished
						// slice, so a flood of generic 1000s cannot crowd it out of the 32-slot set.
						// A matrix whose result becomes the divide's denominator is a projection by
						// definition; a matrix that merely sits at a decodable address is not.
						//
						// 1500 sits deliberately between the generic slice band (1000/999) and the
						// pinned re-read (2000) and the title-specific fixed block (3000), so it
						// wins the eviction race against generic slices and loses it to the two
						// candidates that carry stronger evidence. It does NOT make the matrix
						// electable -- PCSX2_REMIX_DIVCAM = 0 still bars sources 6 and 7 from the
						// election outright. It only gets it measured.
						//
						// DEFAULT 0, i.e. the flat 1000 that shipped, and this is honest about its
						// own value: MEASURED on the 2026-08-16 10:19 session with SLICEAUTO = 0,
						// the per-frame candidate set peaked at 20 of 32 occupied and never once
						// filled, so insert_candidate's `score <= worst -> return` rule never fired
						// and this knob would have changed NOTHING. It matters only when the set
						// saturates, which is what SLICEAUTO = 1 does -- that session carried 778
						// slice-auto rows beside 738 slice. Turning it on CHANGES THE PICTURE by
						// the usual route: a different candidate set reaches the GS side, and
						// sources 1/2/3 that get displaced are electable even though 7 is not.
						//
						// Read live per kick, alongside slice_auto/slice_vf above, never latched.
						const bool div_evidence = (flags & candidate_flag_feeds_div) != 0;
						const float base = (div_evidence && div_score_enabled) ? 1500.f : 1000.f;

						// No data-memory address exists for this one. 0xFFFFFFFF is the module's
						// own "no pin" value, so nothing downstream can mistake it for an offset
						// and try to re-read 64 bytes from it.
						insert_candidate(m, ranked(base, m), s_no_pin, start_pc, ucode, 7, flags);
						++s_frame.sliced_published;
					}

					continue;
				}

				if (!matrix.resolvable)
					continue;

				if (matrix.base_auto)
				{
					if (!slice_auto)
						continue;

					if (read_sliced_matrix(mem, matrix, false, tops, m, offset) && finite_window(m))
					{
						insert_candidate(m, ranked(1000.f, m), offset, start_pc, ucode, 6, flags);
						++s_frame.sliced_published;
					}

					// No TOPS hypothesis: the delta is measured against the live register, so
					// substituting a different base for it is not a hypothesis, it is nonsense.
					continue;
				}

				if (read_sliced_matrix(mem, matrix, false, tops, m, offset) && finite_window(m))
				{
					insert_candidate(m, ranked(1000.f, m), offset, start_pc, ucode, 1, flags);
					if (ucode == 0xd74d4042a48b1ba8ULL)
						capture_transform_probe(mem, offset, ucode);
					++s_frame.sliced_published;
				}

				// Second hypothesis for a VIF-relative base: if the program indexed off a
				// register it loaded from XTOP, the live VI is right, but a program that
				// recomputed that register after the load would not be -- so offer the
				// current TOPS bank as well and let the GS side arbitrate.
				if (matrix.rows[0].vi_base != 0 &&
					read_sliced_matrix(mem, matrix, true, tops, m, offset) && finite_window(m))
				{
					insert_candidate(m, ranked(999.f, m), offset, start_pc, ucode, 2, flags);
					++s_frame.sliced_published;
				}
			}
		}

		// The exhaustive shape scan. Measured on R6 3: 52,285,410 windows examined for
		// 4,179 accepted cameras, every one of which came from the back-slice above and none
		// from here. It stays available as the fallback for a title whose microcode does not
		// decode, but it is off by default -- an untargeted sweep of all 16 KB on every kick is
		// far too expensive to run speculatively once the deterministic path is answering.
		for (u32 offset = 0; s_scan_windows && (offset + s_matrix_bytes) <= VU1_MEMSIZE; offset += s_qword)
		{
			++s_frame.windows_examined;

			float m[16];
			std::memcpy(m, mem + offset, sizeof(m));

			if (!finite_window(m))
				continue;

			float row_score = 0.0f;
			float column_score = 0.0f;
			const bool as_row = shape_test(m, false, row_score);
			const bool as_column = shape_test(m, true, column_score);

			if (!as_row && !as_column)
				continue;

			++s_frame.windows_survived;

			// The GS side tries both majorness hypotheses regardless; the better of the two
			// scores is what ranks this window against the others.
			const float score = std::max(as_row ? row_score : 0.0f, as_column ? column_score : 0.0f);
			if (score < 0.5f)
				continue;

			insert_candidate(m, score, offset, start_pc, ucode);
		}

		// The pinned window goes in unconditionally and at the top of the ranking, because the
		// address it sits at is evidence the shape score cannot supply: a camera was actually
		// recovered from there.
		// Bounded to the first few kicks of a frame: the address is re-uploaded per frame but
		// its contents are rewritten between kicks, and letting every kick contribute a
		// 1000-scored entry would evict the whole rest of the set.
		const u32 pinned = s_pinned_offset.load(std::memory_order_relaxed);
		if (pinned != s_no_pin && s_frame.kicks_scanned <= pinned_kick_window() &&
			(pinned + s_matrix_bytes) <= VU1_MEMSIZE)
		{
			float m[16];
			std::memcpy(m, mem + pinned, sizeof(m));

			// Source 3, not the default 0. This candidate is the address a back-slice already
			// resolved, re-read each frame -- calling it a window-scan hit made 'accept (sliced
			// N)' under-report by 20x on SOCOM (317 of 6,439 accepts looked unattributed when in
			// fact all of them were the same sliced matrix), and cost it the non-scan ranking
			// bonus that says an address a camera was actually recovered from outranks a shape
			// score.
			// 2000, not 1000. The pin is the one candidate we have positive evidence for -- a
			// camera was actually recovered from this address -- and at 1000 it was being refused
			// by the `score <= worst` rule the moment the set filled with other 1000s, which is
			// exactly when it is most needed. The structural bonus rides on top so that, among the
			// several different things the address holds across a frame, the kick where it really
			// does hold the camera outranks the kicks where it does not.
			if (usable_matrix(m))
			{
				insert_candidate(m, ranked(2000.f, m), pinned, start_pc, ucode, 3);

				std::memcpy(s_pin_retained, m, sizeof(s_pin_retained));
				s_pin_retained_valid = true;
				s_pin_retained_generation = generation;
			}
			else if (s_pin_retained_valid && (pin_hold_frames() > 0) &&
					 (generation >= s_pin_retained_generation) &&
					 ((generation - s_pin_retained_generation) <= pin_hold_frames()))
			{
				// 1999, not 2000: a fresh read of the address must always outrank a remembered
				// one, but a remembered one still has to outrank every slice candidate or it is
				// back to being evicted by the flood this whole path exists to survive.
				insert_candidate(s_pin_retained, ranked(1999.f, s_pin_retained), pinned, start_pc,
					ucode, 3);
			}
		}

		trace("scanned", s_frame.windows_survived, s_frame.count);

		// Publish every scanned kick, empty set included: a frame in which nothing qualified
		// must not present the previous frame's winner as its own.
		publish();

		trace("published", s_seq.load(std::memory_order_relaxed), 0);
	}

	void DropPublished()
	{
		// Called from the GS thread when the guest's scene has been replaced wholesale (a
		// save-state load). The published set describes the *old* scene, and the world camera
		// solved from it would un-project the new scene's vertices into nonsense.
		//
		// Deliberately NOT a reset of the seqlock: the VU thread may be mid-publish, and
		// zeroing s_seq under it would make the writer's seq+2 land on an odd value and wedge
		// every future Latch. Instead the GS side records the sequence number it must see
		// before it will trust a frame again. Worst case that discards one publish either
		// side of the load, which is exactly the right conservative behaviour.
		s_drop_before_seq.store(s_seq.load(std::memory_order_relaxed) + 2, std::memory_order_relaxed);
	}

	bool Latch(Frame& out)
	{
		bool have = false;

		for (u32 attempt = 0; attempt < 16; ++attempt)
		{
			const u32 seq = s_seq.load(std::memory_order_acquire);
			if (seq == 0)
				break; // nothing has ever been published

			if (seq < s_drop_before_seq.load(std::memory_order_relaxed))
				break; // published before a scene discontinuity -- stale by construction

			if (seq & 1u)
				continue; // a publish is in flight

			std::memcpy(&out, &s_published, sizeof(Frame));
			std::atomic_thread_fence(std::memory_order_acquire);

			if (s_seq.load(std::memory_order_relaxed) == seq)
			{
				have = true;
				break;
			}
		}

		s_generation.fetch_add(1, std::memory_order_relaxed);
		return have;
	}
} // namespace RemixVU1Capture
