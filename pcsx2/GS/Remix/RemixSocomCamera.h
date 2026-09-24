// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Remix/RemixTransforms.h"

#include <cstddef>

namespace remix_ps2::socom_camera
{
	// SCUS-97545 retail layout, verified from its EE code and two initialized savestates.
	inline constexpr u32 combined_assault_crc = 0xD7CFDCCF;

	struct Snapshot
	{
		mat4 view{};
		mat4 world_to_screen{};
		mat4 world_to_clip{};
		mat4 clip_to_screen{};
		u32 camera_address = 0;
		u32 frame_setup_address = 0;
		u32 screen_left = 0;
		u32 screen_top = 0;
		u32 screen_width = 0;
		u32 screen_height = 0;
	};

	struct Viewport
	{
		float ofx = 0.f;
		float ofy = 0.f;
		u32 width = 0;
		u32 height = 0;
	};

	struct Camera
	{
		mat4 view{};
		// The guest raster depth is left intact here; rebuild_projection_z supplies D3D depth.
		mat4 projection{};
		mat4 normalized{};
		clip_solver solver{};
		float position[3]{};
	};

	struct Calibration
	{
		mat4 inverse_culling_projection{};
		mat4 raster_projection{};
	};

	// Call only on the EE producer, then transport the value with the corresponding VU kick.
	bool ReadSnapshot(const u8* memory, size_t memory_size, u32 crc, Snapshot& out);
	bool BuildCamera(const Snapshot& snapshot, const Viewport& viewport, Camera& out);
	// The recognized retail microprogram retains this raster transform in VU qwords 4..7.
	bool BuildRaster(const mat4& captured_raster, const Viewport& viewport, Camera& out);
	bool IsRaster(const mat4& captured_raster);
	// Recognizes the witnessed raster program layout, including its independent culling path.
	// Combine this with a SOCOM title guard; it does not identify the loaded game.
	struct RasterProgramWitness
	{
		u32 start = 0;
		u32 divide = 0;
		u32 conversion = 0;
		bool valid = false;
	};
	bool MatchesRasterProgram(const u8* micro_memory, size_t micro_memory_size, RasterProgramWitness* witness = nullptr);
	bool ValidateRasterProgramWitness(const u8* micro_memory, size_t micro_memory_size, const RasterProgramWitness& witness);
	bool MakeCalibration(const Snapshot& snapshot, Calibration& out);
	// The camera pose comes exclusively from this kick's captured matrix; the calibration
	// cancels the reference pose. A different lens or non-camera block fails the rigid check.
	bool BuildCaptured(const mat4& captured_culling, const Calibration& calibration,
		const Viewport& viewport, Camera& out);

	// FrameSetup is uploaded to VU qwords 4..15 before world raster kicks. Matching both
	// transforms rejects stale snapshots and independent shadow/sky setup blocks.
	bool MatchesVU(const Snapshot& snapshot, const u8* vu_memory, size_t vu_memory_size);
}
