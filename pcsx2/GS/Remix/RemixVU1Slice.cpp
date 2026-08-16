// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Remix/RemixVU1Slice.h"

#include "VUmicro.h"

#include "fmt/format.h"

#include <algorithm>
#include <cstring>
#include <iterator>

namespace RemixVU1Slice
{
	namespace
	{
		// --- field extraction, mirroring microVU_Misc.h:77-79 and :105 --------------------
		__fi u32 field_ft(u32 code) { return (code >> 16) & 0x1F; }
		__fi u32 field_fs(u32 code) { return (code >> 11) & 0x1F; }
		__fi u32 field_fd(u32 code) { return (code >> 6) & 0x1F; }
		__fi u32 field_is(u32 code) { return (code >> 11) & 0xF; }
		__fi u32 field_it(u32 code) { return (code >> 16) & 0xF; }
		__fi u32 field_id(u32 code) { return (code >> 6) & 0xF; }
		__fi u32 field_bc(u32 code) { return code & 0x3; }

		// Signed 11-bit qword displacement, microVU_Misc.h:115.
		__fi s32 field_imm11(u32 code)
		{
			return (code & 0x400) ? static_cast<s32>(0xFFFFFC00u | (code & 0x3FF)) : static_cast<s32>(code & 0x3FF);
		}

		// --- upper-op classification -------------------------------------------------------
		enum class Upper : u8
		{
			Other = 0,
			MulaBc, // MULAbc  ACC = fs * ft[bc]
			MaddaBc, // MADDAbc ACC = ACC + fs * ft[bc]
			MaddBc, // MADDbc  fd  = ACC + fs * ft[bc]
			Clip, // CLIP    fs, ft.w
		};

		Upper classify_upper(u32 code, u32& out_bc)
		{
			const u32 op = code & 0x3F;

			if (op >= 8 && op <= 11) // MADDx/y/z/w -- microVU_Tables.inl:125
			{
				out_bc = op - 8;
				return Upper::MaddBc;
			}

			if (op >= 0x3C) // the four FD tables -- microVU_Tables.inl:138
			{
				const u32 sub = (code >> 6) & 0x1F;
				out_bc = op - 0x3C; // the table index IS the broadcast component

				if (sub == 6) // MULAx/y/z/w   :143 :154 :165 :176
					return Upper::MulaBc;

				if (sub == 2) // MADDAx/y/z/w  :142 :153 :164 :175
					return Upper::MaddaBc;

				if (sub == 7 && op == 0x3F) // CLIP -- FD_11 index 7, :176
					return Upper::Clip;
			}

			return Upper::Other;
		}

		// --- lower-op classification -------------------------------------------------------
		LoadKind classify_load(u32 code)
		{
			const u32 op = code >> 25; // microVU_Tables.inl:200

			if (op == 0) // LQ -- mVULOWER_OPCODE[0], :25
				return LoadKind::LQ;

			if (op == 0x40) // mVULowerOP, :41
			{
				const u32 low = code & 0x3F;
				const u32 sub = (code >> 6) & 0x1F;

				if (low == 0x3C && sub == 13) // T3_00[13] = LQI, :63
					return LoadKind::LQI;

				if (low == 0x3E && sub == 13) // T3_10[13] = LQD, :85
					return LoadKind::LQD;
			}

			return LoadKind::None;
		}

		// DIV lives at T3_00[14] (microVU_Tables.inl:63) and reads VF[fs].fsf / VF[ft].ftf.
		bool is_div(u32 code)
		{
			return (code >> 25) == 0x40 && (code & 0x3F) == 0x3C && ((code >> 6) & 0x1F) == 14;
		}

		// XGKICK -- T3_00[27], microVU_Tables.inl:67. The GIF packet address is VI[is].
		bool is_xgkick(u32 code, u32& out_vi)
		{
			if ((code >> 25) != 0x40 || (code & 0x3F) != 0x3C || ((code >> 6) & 0x1F) != 27)
				return false;

			out_vi = field_is(code);
			return true;
		}

		// The VF register an upper op writes, or 32 for "none".
		//
		// Everything in mVU_UPPER_OPCODE[0x00..0x2F] writes _Fd_ (microVU_Tables.inl:122-134).
		// The four FD tables at 0x3C..0x3F are the accumulator forms and write no VF at all --
		// except ITOF/FTOI at sub 4/5 and ABS at FD_01[7], which write _Ft_. NOP is FD_11[11],
		// which is the filler instruction SOCOM's microcode is largely made of, so getting this
		// wrong would retire every open chain on the first pad instruction after it.
		u32 upper_writes_vf(u32 code)
		{
			const u32 op = code & 0x3F;

			if (op < 0x3C)
				return field_fd(code);

			const u32 sub = (code >> 6) & 0x1F;

			if (sub == 4 || sub == 5) // ITOF / FTOI -- microVU_Tables.inl:142-183
				return field_ft(code);

			if (sub == 7 && op == 0x3D) // ABS -- FD_01[7], :155
				return field_ft(code);

			return 32; // ADDA/SUBA/MADDA/MSUBA/MULA/OPMULA -> ACC, CLIP -> flags, NOP -> nothing
		}

		enum class Branch : u8
		{
			None = 0,
			Conditional, // IBEQ/IBNE/IBLTZ/IBGTZ/IBLEZ/IBGEZ -- falls through when not taken
			Unconditional, // B/BAL/JR/JALR -- straight-line decoding stops meaning anything here
		};

		// mVULOWER_OPCODE indices, microVU_Tables.inl:25-58.
		Branch classify_branch(u32 code)
		{
			switch (code >> 25)
			{
				case 32: // B
				case 33: // BAL
				case 36: // JR
				case 37: // JALR
					return Branch::Unconditional;
				case 40: // IBEQ
				case 41: // IBNE
				case 44: // IBLTZ
				case 45: // IBGTZ
				case 46: // IBLEZ
				case 47: // IBGEZ
					return Branch::Conditional;
				default:
					return Branch::None;
			}
		}

		// The signed qword adjustment an instruction applies to VI[reg] as a side effect of an
		// addressing mode. LQI/LQD adjust their source base; SQI/SQD adjust their destination.
		s32 vi_auto_adjust(u32 code, u32 reg)
		{
			if ((code >> 25) != 0x40)
				return 0;

			const u32 low = code & 0x3F;
			const u32 sub = (code >> 6) & 0x1F;

			if (sub != 13)
				return 0;

			switch (low)
			{
				case 0x3C: // LQI  -- T3_00[13]
					return (field_is(code) == reg) ? 1 : 0;
				case 0x3D: // SQI  -- T3_01[13], destination is _It_
					return (field_it(code) == reg) ? 1 : 0;
				case 0x3E: // LQD  -- T3_10[13]
					return (field_is(code) == reg) ? -1 : 0;
				case 0x3F: // SQD  -- T3_11[13], destination is _It_
					return (field_it(code) == reg) ? -1 : 0;
				default:
					return 0;
			}
		}

		// The VI register an instruction overwrites wholesale, or 16 for "none". Anything here
		// destroys the arithmetic relationship between a load's base and the kick's base, which is
		// the whole reason a delta can be trusted at all. Auto-adjusts are deliberately excluded:
		// those are accounted for exactly, not treated as clobbers.
		u32 vi_clobber(u32 code)
		{
			const u32 op = code >> 25;

			switch (op)
			{
				case 4: // ILW
				case 8: // IADDIU
				case 9: // ISUBIU
					return field_it(code);
				case 33: // BAL  -- writes the link register
				case 37: // JALR
					return field_it(code);
				default:
					break;
			}

			if (op != 0x40)
				return 16;

			const u32 low = code & 0x3F;

			switch (low)
			{
				case 0x30: // IADD  -- mVULowerOP_OPCODE[48], microVU_Tables.inl:116
				case 0x31: // ISUB
				case 0x34: // IAND
				case 0x35: // IOR
					return field_id(code);
				case 0x32: // IADDI
					return field_it(code);
				default:
					break;
			}

			const u32 sub = (code >> 6) & 0x1F;

			if (low == 0x3C && (sub == 15 || sub == 26)) // MTIR, XTOP
				return field_it(code);
			if (low == 0x3D && sub == 26) // XITOP
				return field_it(code);
			if (low == 0x3E && sub == 15) // ILWR
				return field_it(code);

			return 16;
		}

		const char* load_name(LoadKind k)
		{
			switch (k)
			{
				case LoadKind::LQ:
					return "LQ";
				case LoadKind::LQI:
					return "LQI";
				case LoadKind::LQD:
					return "LQD";
				default:
					return "-";
			}
		}

		constexpr char s_bc_names[4] = {'x', 'y', 'z', 'w'};

		// A chain under construction.
		struct ActiveChain
		{
			bool open = false;
			u32 mask = 0; // which broadcast components have been seen
			u32 vertex_vf = 0;
			u32 start_pc = 0;
			u32 age = 0; // instructions since the chain opened
			RowLoad rows[4] = {};
			u32 row_vf[4] = {};
			bool row_found[4] = {};
		};

		// How far the backward pairing will look for a kick. One block's worth: SOCOM's is 5
		// instructions, and past a few dozen the claim "this load indexes off that kick's base"
		// stops being supported by anything.
		constexpr u32 s_back_limit = 64;

		// Ties one row load to the XGKICK its base register is readable at, and works out the
		// qword adjustment that turns the register's value AT THAT KICK back into the address the
		// load actually used.
		//
		// Forward first, because a program that loads a matrix and then kicks (the LQI streaming
		// idiom) has advanced the register past the matrix by kick time and that is the case which
		// has never worked. Backward second, for a program that sets a base, kicks from it, and
		// then reads the matrix out of the same block by immediate -- there the register still
		// holds the block base at the kick and the delta is 0, which reproduces the old behaviour
		// exactly.
		//
		// Either direction stops at anything that overwrites the register, because the arithmetic
		// relationship is what makes this deterministic rather than a guess.
		void pair_row_to_kick(const u32* words, u32 start_pc, u32 end_pc, RowLoad& row)
		{
			const u32 reg = row.vi_base;
			const u32 load_pc = row.pc;

			// VI00 is hardwired to zero, so nothing adjusts it and nothing can be recovered
			// relative to it -- but an LQ off VI00 is an absolute address and already correct.
			if (reg == 0)
			{
				row.vi_delta = 0;
				row.kick_dir = (row.kind == LoadKind::LQ) ? KickDir::Backward : KickDir::None;
				return;
			}

			// --- forward ---------------------------------------------------------------------
			s32 acc = vi_auto_adjust(words[load_pc / 4], reg); // the load's own post/pre adjust
			u32 crossed = 0;

			for (u32 pc = load_pc + 8; (pc + 8) <= end_pc; pc += 8)
			{
				const u32 lower = words[pc / 4];

				u32 kick_vi = 0;
				if (is_xgkick(lower, kick_vi) && kick_vi == reg)
				{
					// VI_at_kick = VI_before_load + acc, so the load's base is VI_at_kick - acc.
					// LQD reads one qword BELOW its pre-adjust base, hence the extra -1.
					row.vi_delta = static_cast<s16>(-acc - ((row.kind == LoadKind::LQD) ? 1 : 0));
					row.kick_pc = static_cast<u16>(pc);
					row.kick_dir = KickDir::Forward;
					row.cond_branches = static_cast<u8>(std::min<u32>(crossed, 255));
					return;
				}

				acc += vi_auto_adjust(lower, reg);

				if (vi_clobber(lower) == reg)
					break;

				const Branch br = classify_branch(lower);
				if (br == Branch::Unconditional)
					break;
				if (br == Branch::Conditional)
					++crossed;
			}

			// --- backward --------------------------------------------------------------------
			s32 bacc = 0;
			crossed = 0;

			for (u32 back = 1; back <= s_back_limit; ++back)
			{
				const u32 offset = back * 8;
				if (offset > load_pc || (load_pc - offset) < start_pc)
					break;

				const u32 pc = load_pc - offset;
				const u32 lower = words[pc / 4];

				if (vi_clobber(lower) == reg)
					break;

				u32 kick_vi = 0;
				if (is_xgkick(lower, kick_vi) && kick_vi == reg)
				{
					// VI_before_load = VI_at_kick + bacc.
					row.vi_delta = static_cast<s16>(bacc - ((row.kind == LoadKind::LQD) ? 1 : 0));
					row.kick_pc = static_cast<u16>(pc);
					row.kick_dir = KickDir::Backward;
					row.cond_branches = static_cast<u8>(std::min<u32>(crossed, 255));
					return;
				}

				bacc += vi_auto_adjust(lower, reg);

				if (classify_branch(lower) != Branch::None)
					++crossed;
			}

			row.vi_delta = 0;
			row.kick_dir = KickDir::None;
		}
	} // namespace

	void Analyze(const u8* micro, u32 start_pc, Program& out)
	{
		out = Program{};

		if (!micro)
			return;

		const u32* const words = reinterpret_cast<const u32*>(micro);

		// Last LQ-family writer of each VF register, as a straight-line approximation. The
		// matrix is loaded before the transform in program order, which is all this needs;
		// branches are ignored exactly as RPCS3's bounded back-slice ignores them.
		RowLoad vf_load[32] = {};

		ActiveChain active;

		// Completed chains still awaiting a CLIP/DIV consumer marker, as a bitmask over items[].
		//
		// This used to be a single `pending` index that was never cleared, so the LAST matrix in a
		// program collected every DIV to the end of the straight-line decode -- across returns,
		// across unrelated subroutines, across anything. On SOCOM Combined Assault that marked its
		// per-object matrix at pc=0x2be0 as feeding the perspective divide on the strength of a
		// `DIV Q, vf00.w, vf22.w` at pc=0x2de8, which is in a different routine reached only
		// through the `JR vi06` at 0x2d38 and divides a vf22 that routine computed itself. The
		// flag is the projection discriminator; a false positive in it is a wrong camera.
		u32 open_mask = 0;
		u32 close_after = 0; // instructions still to run before an unconditional transfer bites

		u32 end_pc = VU1_PROGSIZE;

		for (u32 pc = start_pc & ~7u; (pc + 8) <= VU1_PROGSIZE; pc += 8)
		{
			const u32 lower = words[pc / 4];
			const u32 upper = words[(pc / 4) + 1];

			++out.instructions;

			// --- upper: chain detection ---------------------------------------------------
			u32 bc = 0;
			const Upper kind = classify_upper(upper, bc);
			const u32 fs = field_fs(upper);
			const u32 ft = field_ft(upper);

			if (kind == Upper::MulaBc)
			{
				// MULA always restarts the accumulator, so it always opens a fresh chain.
				active = ActiveChain{};
				active.open = true;
				active.vertex_vf = ft;
				active.start_pc = pc;
				++out.chains_started;
			}

			if (active.open && (kind == Upper::MulaBc || kind == Upper::MaddaBc || kind == Upper::MaddBc))
			{
				// vf00 is the constant (0,0,0,1), so a w term written as MADDw fd, m3, vf00
				// is the same chain -- it is how a point with an implicit w of 1 is folded in.
				if (ft == active.vertex_vf || ft == 0)
				{
					active.rows[bc] = vf_load[fs];
					active.row_vf[bc] = fs;
					active.row_found[bc] = (vf_load[fs].kind != LoadKind::None);
					active.mask |= (1u << bc);
				}
			}

			if (active.open && kind == Upper::MaddBc && active.mask == 0xF)
			{
				++out.chains_complete;

				u32 found = 0;
				for (u32 i = 0; i < 4; ++i)
					found += active.row_found[i] ? 1u : 0u;

				// Wholly register-resident: not one row of this matrix is loaded anywhere in the
				// program. It was put in the register file by a different microprogram and is
				// still there, which is how a shared view-projection gets carried on VU1 -- and
				// it is why looking only for data-memory addresses can miss the camera entirely.
				const bool register_rows = (found == 0);

				if (register_rows)
					++out.chains_register;

				if (found != 0 && found != 4)
				{
					// A genuine partial slice. Nothing can be read for it either way.
					++out.chains_unsliced;
				}
				else if (out.count < max_matrices)
				{
					Matrix& m = out.items[out.count++];
					std::memcpy(m.rows, active.rows, sizeof(m.rows));
					m.chain_pc = static_cast<u16>(active.start_pc);
					m.close_pc = static_cast<u16>(pc);
					m.result_vf = static_cast<u8>(field_fd(upper));
					m.vertex_vf = static_cast<u8>(active.vertex_vf);
					m.register_rows = register_rows;

					for (u32 i = 0; i < 4; ++i)
						m.rows[i].vf_reg = static_cast<u8>(active.row_vf[i]);

					// Register rows need no address at all; the memory rows are resolved by the
					// kick-pairing pass once the program's extent is known.
					m.resolvable = register_rows;

					open_mask |= (1u << (out.count - 1));
				}

				active = ActiveChain{};
			}
			else if (active.open)
			{
				// A chain that has not closed within a handful of instructions is not one.
				if (++active.age > 12)
					active = ActiveChain{};
			}

			// --- consumer markers, so the GS side can prefer clip-space results ------------
			//
			// Bounded to the basic block, and to the lifetime of the chain's own result. A DIV
			// reached only through a branch is dividing whatever THAT path computed, and a DIV
			// after the result register has been rewritten is dividing the rewrite.
			for (u32 i = 0; i < out.count; ++i)
			{
				if ((open_mask & (1u << i)) == 0)
					continue;

				Matrix& m = out.items[i];

				if (kind == Upper::Clip && fs == m.result_vf)
					m.feeds_clip = true;

				if (is_div(lower) && (field_ft(lower) == m.result_vf || field_fs(lower) == m.result_vf))
					m.feeds_div = true;

				if (pc > m.close_pc && upper_writes_vf(upper) == m.result_vf)
					open_mask &= ~(1u << i);
			}

			// An unconditional transfer ends the block -- but VU branches have a delay slot, so
			// the instruction after it still runs and still counts.
			if (close_after > 0 && --close_after == 0)
				open_mask = 0;
			else if (close_after == 0 && classify_branch(lower) == Branch::Unconditional)
				close_after = 1;

			// --- lower: record loads ------------------------------------------------------
			const LoadKind lk = classify_load(lower);
			if (lk != LoadKind::None)
			{
				RowLoad& slot = vf_load[field_ft(lower)];
				slot = RowLoad{};
				slot.kind = lk;
				slot.vi_base = static_cast<u8>(field_is(lower));
				slot.imm = (lk == LoadKind::LQ) ? static_cast<s16>(field_imm11(lower)) : 0;
				slot.pc = static_cast<u16>(pc);
			}

			end_pc = pc + 8;

			// E bit -- end of the microprogram (VU1microInterp.cpp:41).
			if (upper & 0x40000000u)
				break;
		}

		// --- pairing pass: turn every memory row into an offset from a readable VI value -------
		for (u32 i = 0; i < out.count; ++i)
		{
			Matrix& m = out.items[i];
			if (m.register_rows)
				continue;

			bool resolvable = true;
			bool base_auto = false;

			for (u32 r = 0; r < 4; ++r)
			{
				RowLoad& row = m.rows[r];
				pair_row_to_kick(words, start_pc & ~7u, end_pc, row);

				if (row.kind != LoadKind::LQ || row.vi_delta != 0)
					base_auto = true;

				// An LQ off a register the pairing could not tie to a kick keeps the historical
				// behaviour -- read the live VI and add the immediate -- because that is what a
				// program which never moves its base is doing anyway. An LQI/LQD cannot fall back
				// on anything: without the count, its address is unknown, not approximate.
				if (row.kick_dir == KickDir::None && row.kind != LoadKind::LQ)
					resolvable = false;
			}

			m.resolvable = resolvable;
			m.base_auto = base_auto;

			if (!resolvable)
				++out.chains_unresolvable;
		}
	}

	std::string Describe(const u8* micro, u32 start_pc, const Program& prog)
	{
		std::string out;

		fmt::format_to(std::back_inserter(out),
			"start_pc=0x{:04x} instructions={} chains: started {} complete {} unsliced {} "
			"unresolvable {} register {} | matrices {}\n",
			start_pc, prog.instructions, prog.chains_started, prog.chains_complete,
			prog.chains_unsliced, prog.chains_unresolvable, prog.chains_register, prog.count);

		if (!micro)
			return out;

		const u32* const words = reinterpret_cast<const u32*>(micro);

		for (u32 i = 0; i < prog.count; ++i)
		{
			const Matrix& m = prog.items[i];

			fmt::format_to(std::back_inserter(out),
				"  matrix {} @pc=0x{:04x} kind={} result=vf{:02d} vertex=vf{:02d} clip={} div={} "
				"resolvable={} auto={}\n",
				i, m.chain_pc, m.register_rows ? "regs" : "mem", m.result_vf, m.vertex_vf,
				m.feeds_clip ? 1 : 0, m.feeds_div ? 1 : 0, m.resolvable ? 1 : 0, m.base_auto ? 1 : 0);

			for (u32 r = 0; r < 4; ++r)
			{
				const RowLoad& row = m.rows[r];

				if (m.register_rows)
				{
					fmt::format_to(std::back_inserter(out),
						"    row {} (bc={}) <- vf{:02d} (register-resident, set by another program)\n",
						r, s_bc_names[r], row.vf_reg);
					continue;
				}

				const char* dir = (row.kick_dir == KickDir::Forward) ?
									  "fwd" :
									  ((row.kick_dir == KickDir::Backward) ? "bwd" : "none");

				fmt::format_to(std::back_inserter(out),
					"    row {} (bc={}) <- {} vi{:02d} imm={} @pc=0x{:04x} delta={} kick=0x{:04x} "
					"dir={} condbr={}\n",
					r, s_bc_names[r], load_name(row.kind), row.vi_base, row.imm, row.pc,
					row.vi_delta, row.kick_pc, dir, row.cond_branches);
			}

			// The chain itself, raw, so the encoding can be checked by hand.
			for (u32 k = 0; k < 6; ++k)
			{
				const u32 pc = m.chain_pc + (k * 8);
				if ((pc + 8) > VU1_PROGSIZE)
					break;

				const u32 lower = words[pc / 4];
				const u32 upper = words[(pc / 4) + 1];

				u32 bc = 0;
				const Upper kind = classify_upper(upper, bc);
				const char* name = "?";
				switch (kind)
				{
					case Upper::MulaBc:
						name = "MULA";
						break;
					case Upper::MaddaBc:
						name = "MADDA";
						break;
					case Upper::MaddBc:
						name = "MADD";
						break;
					case Upper::Clip:
						name = "CLIP";
						break;
					default:
						break;
				}

				const char suffix = (kind == Upper::MulaBc || kind == Upper::MaddaBc || kind == Upper::MaddBc) ?
				                        s_bc_names[bc] :
				                        ' ';

				fmt::format_to(std::back_inserter(out),
					"    pc=0x{:04x} U=0x{:08x} L=0x{:08x}  {}{} fd=vf{:02d} fs=vf{:02d} ft=vf{:02d}"
					"  | L:{} ft=vf{:02d} vi{:02d} imm={}\n",
					pc, upper, lower, name, suffix,
					field_fd(upper), field_fs(upper), field_ft(upper),
					load_name(classify_load(lower)), field_ft(lower), field_is(lower), field_imm11(lower));
			}
		}

		return out;
	}
} // namespace RemixVU1Slice
