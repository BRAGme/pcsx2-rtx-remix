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
	inline constexpr u32 max_matrices = 6;

	enum class LoadKind : u8
	{
		None = 0,
		LQ, // LQ  VFt, imm(VIs)   -- address is VI[s] + imm, resolvable
		LQI, // LQI VFt, (VIs)++    -- VI has moved on by capture time
		LQD, // LQD VFt, --(VIs)
	};

	struct RowLoad
	{
		LoadKind kind = LoadKind::None;
		u8 vi_base = 0;
		s16 imm = 0; // in qwords
		u16 pc = 0; // byte PC of the load, for the dump
	};

	struct Matrix
	{
		RowLoad rows[4]; // indexed by broadcast component: 0 = x .. 3 = w
		u16 chain_pc = 0; // byte PC of the chain's first MULA
		u8 result_vf = 0;
		u8 vertex_vf = 0;
		bool feeds_clip = false; // the result is later CLIPped -> it is clip space
		bool feeds_div = false; // the result feeds a DIV -> the perspective divide
		bool resolvable = false; // every row back-sliced to an LQ with a usable base
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
		u32 chains_unresolvable = 0; // sliced, but the loads were LQI/LQD
	};

	// Straight-line decode from start_pc to the first E-bit (or the end of micro memory).
	void Analyze(const u8* micro, u32 start_pc, Program& out);

	// Disassembly of the sliced instructions -- the chains and the loads feeding them.
	// This is the artefact a null result ships, the way RPCS3 dumps the HPOS slice for a
	// program it could not classify.
	std::string Describe(const u8* micro, u32 start_pc, const Program& prog);
} // namespace RemixVU1Slice
