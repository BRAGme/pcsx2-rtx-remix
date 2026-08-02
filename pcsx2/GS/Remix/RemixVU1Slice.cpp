// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Remix/RemixVU1Slice.h"

#include "VUmicro.h"

#include "fmt/format.h"

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
			u32 row_pc[4] = {};
			bool row_found[4] = {};
		};
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

		// Completed chains awaiting a CLIP/DIV consumer marker.
		u32 pending = 0;

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
					active.row_found[bc] = (vf_load[fs].kind != LoadKind::None);
					active.mask |= (1u << bc);
				}
			}

			if (active.open && kind == Upper::MaddBc && active.mask == 0xF)
			{
				++out.chains_complete;

				bool sliced = true;
				bool resolvable = true;
				for (u32 i = 0; i < 4; ++i)
				{
					if (!active.row_found[i])
						sliced = false;
					else if (active.rows[i].kind != LoadKind::LQ)
						resolvable = false;
				}

				if (!sliced)
				{
					++out.chains_unsliced;
				}
				else
				{
					if (!resolvable)
						++out.chains_unresolvable;

					if (out.count < max_matrices)
					{
						Matrix& m = out.items[out.count++];
						std::memcpy(m.rows, active.rows, sizeof(m.rows));
						m.chain_pc = static_cast<u16>(active.start_pc);
						m.result_vf = static_cast<u8>(field_fd(upper));
						m.vertex_vf = static_cast<u8>(active.vertex_vf);
						m.resolvable = resolvable;
						pending = out.count;
					}
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
			if (pending > 0)
			{
				Matrix& m = out.items[pending - 1];

				if (kind == Upper::Clip && fs == m.result_vf)
					m.feeds_clip = true;

				if (is_div(lower) && (field_ft(lower) == m.result_vf || field_fs(lower) == m.result_vf))
					m.feeds_div = true;
			}

			// --- lower: record loads ------------------------------------------------------
			const LoadKind lk = classify_load(lower);
			if (lk != LoadKind::None)
			{
				RowLoad& slot = vf_load[field_ft(lower)];
				slot.kind = lk;
				slot.vi_base = static_cast<u8>(field_is(lower));
				slot.imm = (lk == LoadKind::LQ) ? static_cast<s16>(field_imm11(lower)) : 0;
				slot.pc = static_cast<u16>(pc);
			}

			// E bit -- end of the microprogram (VU1microInterp.cpp:41).
			if (upper & 0x40000000u)
				break;
		}
	}

	std::string Describe(const u8* micro, u32 start_pc, const Program& prog)
	{
		std::string out;

		fmt::format_to(std::back_inserter(out),
			"start_pc=0x{:04x} instructions={} chains: started {} complete {} unsliced {} "
			"unresolvable {} | matrices {}\n",
			start_pc, prog.instructions, prog.chains_started, prog.chains_complete,
			prog.chains_unsliced, prog.chains_unresolvable, prog.count);

		if (!micro)
			return out;

		const u32* const words = reinterpret_cast<const u32*>(micro);

		for (u32 i = 0; i < prog.count; ++i)
		{
			const Matrix& m = prog.items[i];

			fmt::format_to(std::back_inserter(out),
				"  matrix {} @pc=0x{:04x} result=vf{:02d} vertex=vf{:02d} clip={} div={} resolvable={}\n",
				i, m.chain_pc, m.result_vf, m.vertex_vf, m.feeds_clip ? 1 : 0, m.feeds_div ? 1 : 0,
				m.resolvable ? 1 : 0);

			for (u32 r = 0; r < 4; ++r)
			{
				const RowLoad& row = m.rows[r];
				fmt::format_to(std::back_inserter(out),
					"    row {} (bc={}) <- {} vi{:02d} imm={} @pc=0x{:04x}\n",
					r, s_bc_names[r], load_name(row.kind), row.vi_base, row.imm, row.pc);
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
