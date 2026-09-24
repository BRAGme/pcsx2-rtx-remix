// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Remix/RemixSocomCamera.h"

#include <cmath>
#include <cstring>

namespace remix_ps2::socom_camera
{
	namespace
	{
		constexpr u32 camera_global_address = 0x00743E10;
		constexpr u32 camera_vtable = 0x006E51E0;
		constexpr u32 camera_view_offset = 0xD0;
		constexpr u32 camera_frame_setup_offset = 0x1D0;
		constexpr u32 camera_screen_offset = 0x420;

		bool Contains(size_t size, u32 address, size_t count)
		{
			return address <= size && count <= size - address;
		}

		u32 ReadWord(const u8* memory, u32 address)
		{
			u32 value;
			std::memcpy(&value, memory + address, sizeof(value));
			return value;
		}

		bool RigidView(const mat4& view)
		{
			if (!mat4_is_finite(view) || !is_affine(view))
				return false;
			for (u32 i = 0; i < 3; ++i)
			{
				for (u32 j = 0; j < 3; ++j)
				{
					float dot = 0.f;
					for (u32 k = 0; k < 3; ++k)
						dot += view.m[k][i] * view.m[k][j];
					if (std::abs(dot - (i == j ? 1.f : 0.f)) > 0.001f)
						return false;
				}
			}
			const float determinant = view.m[0][0] * (view.m[1][1] * view.m[2][2] - view.m[1][2] * view.m[2][1]) -
				view.m[0][1] * (view.m[1][0] * view.m[2][2] - view.m[1][2] * view.m[2][0]) +
				view.m[0][2] * (view.m[1][0] * view.m[2][1] - view.m[1][1] * view.m[2][0]);
			return std::abs(determinant - 1.f) <= 0.001f;
		}
	}

	bool ReadSnapshot(const u8* memory, size_t memory_size, u32 crc, Snapshot& out)
	{
		out = {};
		if (!memory || crc != combined_assault_crc ||
			!Contains(memory_size, camera_global_address, sizeof(u32)))
			return false;

		Snapshot snapshot{};
		snapshot.camera_address = ReadWord(memory, camera_global_address);
		if ((snapshot.camera_address & 0xF) != 0 || snapshot.camera_address == 0 ||
			!Contains(memory_size, snapshot.camera_address, camera_screen_offset + 16) ||
			ReadWord(memory, snapshot.camera_address) != camera_vtable ||
			memory[snapshot.camera_address + 0x90] != 10)
			return false;

		snapshot.frame_setup_address = ReadWord(memory, snapshot.camera_address + camera_frame_setup_offset);
		if ((snapshot.frame_setup_address & 0xF) != 0 || snapshot.frame_setup_address == 0 ||
			!Contains(memory_size, snapshot.frame_setup_address, 0xE0) ||
			ReadWord(memory, snapshot.frame_setup_address) != 0x10000018 ||
			ReadWord(memory, snapshot.frame_setup_address + 0x1C) != 0x6C170004)
			return false;

		std::memcpy(&snapshot.view, memory + snapshot.camera_address + camera_view_offset, sizeof(mat4));
		std::memcpy(&snapshot.world_to_screen, memory + snapshot.frame_setup_address + 0x20, sizeof(mat4));
		std::memcpy(&snapshot.world_to_clip, memory + snapshot.frame_setup_address + 0x60, sizeof(mat4));
		std::memcpy(&snapshot.clip_to_screen, memory + snapshot.frame_setup_address + 0xA0, sizeof(mat4));
		if (!RigidView(snapshot.view) || !mat4_is_finite(snapshot.world_to_screen) ||
			!mat4_is_finite(snapshot.world_to_clip) || !mat4_is_finite(snapshot.clip_to_screen))
			return false;

		// Update builds both transforms from this view. Their w columns remain native eye
		// depth, while their x/y columns use different raster and guard-band scales.
		for (u32 i = 0; i < 4; ++i)
		{
			if (std::abs(snapshot.world_to_screen.m[i][3] - snapshot.view.m[i][2]) > 0.001f ||
				std::abs(snapshot.world_to_clip.m[i][3] - snapshot.view.m[i][2]) > 0.001f)
				return false;
		}

		const u32 screen = snapshot.camera_address + camera_screen_offset;
		snapshot.screen_left = ReadWord(memory, screen);
		snapshot.screen_top = ReadWord(memory, screen + 4);
		const u32 right = ReadWord(memory, screen + 8);
		const u32 bottom = ReadWord(memory, screen + 12);
		if (right <= snapshot.screen_left || bottom <= snapshot.screen_top ||
			right - snapshot.screen_left > 4096 || bottom - snapshot.screen_top > 4096)
			return false;
		snapshot.screen_width = right - snapshot.screen_left;
		snapshot.screen_height = bottom - snapshot.screen_top;
		out = snapshot;
		return true;
	}

	bool BuildCamera(const Snapshot& snapshot, const Viewport& viewport, Camera& out)
	{
		out = {};
		if (!viewport.width || !viewport.height || !std::isfinite(viewport.ofx) ||
			!std::isfinite(viewport.ofy) || !RigidView(snapshot.view) ||
			!mat4_is_finite(snapshot.world_to_screen))
			return false;

		// W2screen emits pixels before VU FTOI4. This inverse is exactly the GS vertex map
		// in OnDrawPrims, including its pixel-center correction. (AI-assisted.)
		mat4 raster = snapshot.world_to_screen;
		for (u32 i = 0; i < 4; ++i)
		{
			raster.m[i][0] *= 16.f;
			raster.m[i][1] *= 16.f;
		}
		const float sx = static_cast<float>(viewport.width) * 8.f;
		const float sy = static_cast<float>(viewport.height) * 8.f;
		Camera camera{};
		camera.view = snapshot.view;
		camera.normalized = normalize_screen_clip(raster, sx, viewport.ofx + sx - 8.f + 0.05f,
			-sy, viewport.ofy + sy - 8.f + 0.05f);
		mat4 view_to_world{};
		if (!mat4_invert(camera.view, view_to_world) || !make_clip_solver(camera.normalized, camera.solver))
			return false;
		camera.projection = mat4_multiply(view_to_world, camera.normalized);
		for (u32 i = 0; i < 3; ++i)
		{
			camera.position[i] = view_to_world.m[3][i];
			if (!std::isfinite(camera.position[i]))
				return false;
		}
		if (!mat4_is_finite(camera.projection))
			return false;
		out = camera;
		return true;
	}

	bool MakeCalibration(const Snapshot& snapshot, Calibration& out)
	{
		out = {};
		mat4 view_to_world{};
		if (!RigidView(snapshot.view) || !mat4_invert(snapshot.view, view_to_world) ||
			!mat4_is_finite(snapshot.world_to_clip) || !mat4_is_finite(snapshot.world_to_screen))
			return false;

		mat4 culling = mat4_multiply(view_to_world, snapshot.world_to_clip);
		mat4 raster = mat4_multiply(view_to_world, snapshot.world_to_screen);
		// Camera::Update constructs these projection entries as zero. Multiplying already
		// rounded world matrices can leave cancellation residue at a distant eye.
		constexpr u32 culling_zero[][2] = {{0, 1}, {0, 2}, {0, 3}, {1, 0}, {1, 2}, {1, 3},
			{2, 0}, {2, 1}, {3, 0}, {3, 1}, {3, 3}};
		for (const auto& entry : culling_zero)
		{
			if (std::abs(culling.m[entry[0]][entry[1]]) > 0.002f)
				return false;
			culling.m[entry[0]][entry[1]] = 0.f;
		}
		constexpr u32 raster_zero[][2] = {{0, 1}, {0, 2}, {0, 3}, {1, 0}, {1, 2}, {1, 3},
			{3, 0}, {3, 1}, {3, 3}};
		for (const auto& entry : raster_zero)
		{
			if (std::abs(raster.m[entry[0]][entry[1]]) > 0.25f)
				return false;
			raster.m[entry[0]][entry[1]] = 0.f;
		}
		if (!(culling.m[0][0] > 0.f) || !(culling.m[1][1] > 0.f) ||
			std::abs(culling.m[2][3] - 1.f) > 0.001f || std::abs(raster.m[2][3] - 1.f) > 0.001f ||
			!mat4_invert(culling, out.inverse_culling_projection))
			return false;
		out.raster_projection = raster;
		return true;
	}

	bool BuildRaster(const mat4& captured_raster, const Viewport& viewport, Camera& out)
	{
		out = {};
		if (!mat4_is_finite(captured_raster))
			return false;
		Snapshot snapshot{};
		snapshot.world_to_screen = captured_raster;
		snapshot.view = mat4_identity();
		float depth_length_squared = 0.f;
		for (u32 i = 0; i < 3; ++i)
			depth_length_squared += captured_raster.m[i][3] * captured_raster.m[i][3];
		if (std::abs(depth_length_squared - 1.f) > 0.001f)
			return false;

		// Raster lens scales are positive in Camera::Update. Reading both axes directly
		// preserves the game's handedness instead of deriving a mirrored right axis.
		for (u32 column = 0; column < 2; ++column)
		{
			double offset = 0.0;
			for (u32 i = 0; i < 3; ++i)
				offset += static_cast<double>(captured_raster.m[i][column]) * captured_raster.m[i][3];
			offset /= depth_length_squared;
			// World raster centers are near the GS nominal pixel origin. Guard-band/culling
			// blocks center at zero and are not interchangeable with the raster lens.
			if (std::abs(offset - 2048.0) > 256.0)
				return false;
			double length_squared = 0.0;
			double axis[3];
			for (u32 i = 0; i < 3; ++i)
			{
				axis[i] = captured_raster.m[i][column] - offset * captured_raster.m[i][3];
				length_squared += axis[i] * axis[i];
			}
			if (!(length_squared > 16.0 * 16.0) || length_squared > 65536.0 * 65536.0)
				return false;
			const double length = std::sqrt(length_squared);
			for (u32 i = 0; i < 3; ++i)
				snapshot.view.m[i][column] = static_cast<float>(axis[i] / length);
		}
		for (u32 i = 0; i < 3; ++i)
			snapshot.view.m[i][2] = captured_raster.m[i][3];
		double depth_scale = 0.0;
		for (u32 i = 0; i < 3; ++i)
			depth_scale += static_cast<double>(captured_raster.m[i][2]) * captured_raster.m[i][3];
		depth_scale /= depth_length_squared;
		// Raster depth's spatial gradient must remain parallel to forward. Its slope
		// changes sign with the native near/far setup, but its eye-origin bias is positive.
		const double depth_bias = captured_raster.m[3][2] - depth_scale * captured_raster.m[3][3];
		if (!(depth_bias > 1.0) || !std::isfinite(depth_scale) || !std::isfinite(depth_bias))
			return false;
		for (u32 i = 0; i < 3; ++i)
		{
			if (std::abs(captured_raster.m[i][2] - depth_scale * captured_raster.m[i][3]) > 0.002)
				return false;
		}
		clip_solver raster_solver{};
		if (!make_clip_solver(captured_raster, raster_solver))
			return false;
		float eye[3];
		solve_world_position(raster_solver, 0.f, 0.f, 0.f, eye);
		for (u32 column = 0; column < 3; ++column)
		{
			snapshot.view.m[3][column] = 0.f;
			for (u32 i = 0; i < 3; ++i)
				snapshot.view.m[3][column] -= eye[i] * snapshot.view.m[i][column];
		}
		return BuildCamera(snapshot, viewport, out);
	}

	bool IsRaster(const mat4& captured_raster)
	{
		Camera camera{};
		// Validation depends only on the native raster block, not on a current host viewport.
		return BuildRaster(captured_raster, {0.f, 0.f, 640, 448}, camera);
	}

	namespace
	{
		// Relative byte offsets from the first load. S1 and CA retain the same linked
		// matrix setup while their later divide/raster instruction addresses differ.
		// Loads 4..7 feed vf9..12 and the full raster multiply; 8..11 feed the clip path.
		constexpr u32 raster_signature[][2] = {
			{0x000, 0x01E90004}, {0x008, 0x01EA0005}, {0x010, 0x01EB0006}, {0x018, 0x01EC0007},
			{0x034, 0x01F149BC}, {0x03C, 0x01F150BD}, {0x044, 0x01F158BE}, {0x05C, 0x01F1604B},
			{0x0A8, 0x01F90008}, {0x0B0, 0x01FA0009}, {0x0B8, 0x01FB000A}, {0x0C0, 0x01FC000B},
			{0x0F4, 0x01F1C9BC}, {0x10C, 0x01F1E34B}};

		bool RasterSetup(const u8* micro, u32 start)
		{
			for (const auto& word : raster_signature)
				if (ReadWord(micro, start + word[0]) != word[1])
					return false;
			return true;
		}

		bool RasterDivide(u32 lower)
		{
			return (lower >> 25) == 0x40 && (lower & 0x3F) == 0x3C && ((lower >> 6) & 31) == 14;
		}

		bool RasterConversion(u32 upper)
		{
			const u32 source = (upper >> 11) & 31;
			return (upper & 0x7FF) == 0x17D && source >= 17 && source == ((upper >> 16) & 31);
		}
	}

	bool ValidateRasterProgramWitness(const u8* micro_memory, size_t micro_memory_size, const RasterProgramWitness& witness)
	{
		const size_t limit = micro_memory_size < 8192 ? micro_memory_size : 8192;
		if (!micro_memory || !witness.valid || limit < 0x110 || (witness.start & 7) != 0 ||
			witness.start > limit - 0x110 || (witness.divide & 7) != 0 || (witness.conversion & 7) != 0 ||
			witness.divide < witness.start + 0x110 || witness.conversion < witness.start + 0x110 ||
			witness.divide > limit - 8 || witness.conversion > limit - 8)
			return false;
		return RasterSetup(micro_memory, witness.start) && RasterDivide(ReadWord(micro_memory, witness.divide)) &&
			RasterConversion(ReadWord(micro_memory, witness.conversion + 4));
	}

	bool MatchesRasterProgram(const u8* micro_memory, size_t micro_memory_size, RasterProgramWitness* witness)
	{
		if (witness)
			*witness = {};
		if (!micro_memory || micro_memory_size < 0x110)
			return false;
		const size_t limit = micro_memory_size < 8192 ? micro_memory_size : 8192;
		for (u32 start = 0; start + 0x110 <= limit; start += 8)
		{
			if (!RasterSetup(micro_memory, start))
				continue;
			bool divide = false;
			bool raster_conversion = false;
			u32 divide_offset = 0;
			u32 conversion_offset = 0;
			for (u32 offset = start + 0x110; offset + 8 <= limit; offset += 8)
			{
				const u32 lower = ReadWord(micro_memory, offset);
				const u32 upper = ReadWord(micro_memory, offset + 4);
				// Decode DIV and FTOI4 rather than keying on one title's instruction address.
				if (RasterDivide(lower))
				{
					divide = true;
					divide_offset = offset;
				}
				if (RasterConversion(upper))
				{
					raster_conversion = true;
					conversion_offset = offset;
				}
				if (divide && raster_conversion)
				{
					if (witness)
						*witness = {start, divide_offset, conversion_offset, true};
					return true;
				}
			}
		}
		return false;
	}

	bool BuildCaptured(const mat4& captured_culling, const Calibration& calibration,
		const Viewport& viewport, Camera& out)
	{
		out = {};
		if (!mat4_is_finite(captured_culling) || !mat4_is_finite(calibration.inverse_culling_projection) ||
			!mat4_is_finite(calibration.raster_projection))
			return false;
		Snapshot snapshot{};
		snapshot.view = mat4_multiply(captured_culling, calibration.inverse_culling_projection);
		if (!RigidView(snapshot.view))
			return false;
		snapshot.world_to_screen = mat4_multiply(snapshot.view, calibration.raster_projection);
		return BuildCamera(snapshot, viewport, out);
	}

	bool MatchesVU(const Snapshot& snapshot, const u8* vu_memory, size_t vu_memory_size)
	{
		return vu_memory && vu_memory_size >= 0x100 &&
			std::memcmp(vu_memory + 0x40, &snapshot.world_to_screen, sizeof(mat4)) == 0 &&
			std::memcmp(vu_memory + 0x80, &snapshot.world_to_clip, sizeof(mat4)) == 0 &&
			std::memcmp(vu_memory + 0xC0, &snapshot.clip_to_screen, sizeof(mat4)) == 0;
	}
}
