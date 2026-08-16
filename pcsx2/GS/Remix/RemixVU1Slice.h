// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

#include <string>

// Deterministic VU1 microcode back-slice: finds where a program's 4x4 transform actually
// lives in VU1 data memory, instead of guessing which of the ~1000 scanned windows is a
// camera.
//
// The idiom this recognises is the canonical VU matrix-vector product,
//
//     MULAx   ACC,       VF[m0], VF[v]x
//     MADDAy  ACC,       VF[m1], VF[v]y
//     MADDAz  ACC,       VF[m2], VF[v]z
//     MADDw   VF[out],   VF[m3], VF[v]w     (VF[v] is often vf00 on the w term)
//
// Two things make it decodable rather than merely plausible. First, the broadcast field
// names the matrix row directly -- the operand broadcast by component i is row i -- so the
// row order comes out of the encoding and not out of instruction order (this is the trick
// RPCS3's match_mad_chain uses, and VU1's _bc_ field maps onto it almost exactly). Second,
// back-slicing the LQ/LQI/LQD that last wrote each VF[mi] yields the row's data-memory
// address, which is what makes the result deterministic.
//
// A row's address is only deterministic relative to something the capture side can read, and
// the one register value it can read is whatever the base VI holds at the XGKICK. Two things
// follow, and both are load-bearing:
//
//   * An LQI/LQD row's base is recoverable by counting the auto-increments the program applies
//     to that register between the load and the kick. The count is static -- it comes out of
//     the instruction stream, not out of a guessed address. SOCOM Combined Assault's mission
//     program needs exactly this: it streams its per-object matrix with four LQIs and then
//     kicks off the SAME register, so VI05 at the kick sits four qwords past row 0.
//   * An LQ row's immediate is only meaningful against the VI value the kick sees, so the pairing
//     runs backwards too: a program that sets its base, kicks, and only then reads the matrix
//     out of that block (SOCOM again, at a different vertex format) pairs to the PRECEDING kick.
//
// Finally, a chain whose four rows were never loaded at all is not a failure -- it is a matrix
// living in the VF register file, put there by a different microprogram. That is how a shared
// view-projection is carried on VU1, and it is recorded here (Matrix::register_rows) so the
// capture side can read vuRegs[1].VF directly instead of looking for an address that does not
// exist.
//
// Encodings are pinned to the in-tree tables rather than to documentation:
//   upper opcode      = code & 0x3f                        microVU_Tables.inl:199
//   MADDbc / MULbc    = 8..11 / 24..27                     microVU_Tables.inl:125,129
//   0x3c..0x3f        -> the four FD tables, sub-opcode (code >> 6) & 0x1f
//                        MADDAbc = 2, MULAbc = 6, ADDAbc = 0    :141-183
//   lower opcode      = code >> 25; LQ = 0, mVULowerOP = 0x40   :24, :41
//   LQI  = T3_00[13],  LQD = T3_10[13]                     microVU_Tables.inl:63, :85
//   _Ft_/_Fs_/_Fd_/_bc_ field extraction                   microVU_Misc.h:77-79, :105
//   instruction layout: word[0] = lower, word[1] = upper   VU1microInterp.cpp:38, :66
//
// microVU.h is deliberately not involved -- it defines microVU0/microVU1 as objects and
// compiles as a single translation unit. Only the encodings are borrowed, by value.
namespace RemixVU1Slice
{
	// Raised from 6 when register-resident chains started being recorded: SOCOM Combined
	// Assault's mission program alone yields 9 (2 memory + 7 register).
	inline constexpr u32 max_matrices = 16;

	enum class LoadKind : u8
	{
		None = 0, // the row is a live VF register, never loaded in this program
		LQ, // LQ  VFt, imm(VIs)   -- address is VI[s] + imm
		LQI, // LQI VFt, (VIs)++    -- reads at VI, then VI += 1
		LQD, // LQD VFt, --(VIs)    -- VI -= 1, then reads at VI
	};

	// How a row's base VI value was tied back to a value the capture side can actually read.
	// The only VI state visible at capture time is whatever the register holds at the XGKICK,
	// so every row has to be expressed relative to that instant.
	enum class KickDir : u8
	{
		None = 0, // no XGKICK on this VI could be paired -- the base is unrecoverable
		Forward, // the kick comes after the load: VI has been advanced past the row
		Backward, // the kick comes before the load: the load indexes off the kick's own base
	};

	struct RowLoad
	{
		LoadKind kind = LoadKind::None;
		u8 vi_base = 0;
		s16 imm = 0; // in qwords
		u16 pc = 0; // byte PC of the load, for the dump

		// The row's data-memory address, in qwords, is
		//
		//     VI[vi_base] read at the XGKICK  +  vi_delta  +  imm
		//
		// vi_delta undoes every auto-increment/decrement applied to that register between the
		// load and the kick, including the load's own. It is 0 for the classic case (an LQ with
		// an immediate and no auto-adjust in between), which is the only case that used to
		// resolve at all.
		s16 vi_delta = 0;
		u16 kick_pc = 0; // byte PC of the XGKICK the delta is measured against
		KickDir kick_dir = KickDir::None;
		u8 cond_branches = 0; // conditional branches swept over: >0 means the delta is a guess
		u8 vf_reg = 0; // for a register-resident row, the VF register holding it
	};

	struct Matrix
	{
		RowLoad rows[4]; // indexed by broadcast component: 0 = x .. 3 = w
		u16 chain_pc = 0; // byte PC of the chain's first MULA
		u16 close_pc = 0; // byte PC of the closing MADD, which writes result_vf
		u8 result_vf = 0;
		u8 vertex_vf = 0;
		bool feeds_clip = false; // the result is later CLIPped -> it is clip space
		bool feeds_div = false; // the result feeds a DIV -> the perspective divide

		// Every row is a live VF register that this program never loads. Such a matrix is set up
		// by some OTHER microprogram and survives in the register file, which is exactly how a
		// shared view-projection is carried on VU1 -- and it is invisible to any slicer that
		// only looks for data-memory addresses.
		bool register_rows = false;

		bool resolvable = false; // every row has a usable base

		// At least one row needed an auto-increment chain, or a non-zero vi_delta, to resolve.
		// Kept separate from `resolvable` so the plain LQ+immediate case, which is the only one
		// that ever worked, keeps behaving exactly as it did.
		bool base_auto = false;
	};

	struct Program
	{
		u32 count = 0;
		Matrix items[max_matrices] = {};

		// Counters, so a program that yields nothing says which stage refused.
		u32 instructions = 0;
		u32 chains_started = 0; // a MULA/MADDA opened a chain
		u32 chains_complete = 0; // all four broadcast components filled
		u32 chains_unsliced = 0; // complete, but a row had no LQ-family writer
		u32 chains_unresolvable = 0; // sliced, but a row's base could not be tied to a kick
		u32 chains_register = 0; // of the unsliced ones, how many were wholly register-resident
	};

	// Straight-line decode from start_pc to the first E-bit (or the end of micro memory).
	void Analyze(const u8* micro, u32 start_pc, Program& out);

	// Disassembly of the sliced instructions -- the chains and the loads feeding them.
	// This is the artefact a null result ships, the way RPCS3 dumps the HPOS slice for a
	// program it could not classify.
	std::string Describe(const u8* micro, u32 start_pc, const Program& prog);
} // namespace RemixVU1Slice
