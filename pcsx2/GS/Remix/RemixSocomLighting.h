// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

#include <span>
#include <string_view>

namespace remix_ps2::socom
{
	inline bool IsTitle(std::string_view serial)
	{
		return serial == "SCUS-97134" || serial == "SCUS-97275" ||
			serial == "SCUS-97474" || serial == "SCUS-97399" || serial == "SCUS-97545";
	}

enum class LightingEvidence : u8
{
	None,
	NativeCodeLayout,
	NativeSaveState,
	NativeVUProgram,
};

struct LightingRig
{
	bool valid = false;
	LightingEvidence evidence = LightingEvidence::None;
	u32 world_identity = 0;
	u32 camera_address = 0;
	char world_name[64] = {};
	// Directions are the direction light travels; colours are native normalized RGB.
	float directions[3][3] = {};
	float colours[3][3] = {};
	float ambient[3] = {};
	bool menu = false;
};

// Call on the EE producer thread and carry the POD result with the camera snapshot.
LightingRig ReadLighting(std::span<const u8> memory, std::string_view serial, u32 crc);

struct MissionSnapshot
{
	bool valid = false;
	bool menu = false;
	u32 world_identity = 0;
	char world_name[64] = {};
	char mission_name[64] = {};
};

MissionSnapshot ReadMission(std::span<const u8> memory, std::string_view serial, u32 crc);

// Shared engine setup loads world lighting at absolute q16..23 before transforming
// it for the current object. Validate the loaded VU program before reading that record.
struct VULightingProgramWitness
{
	u32 normals = 0;
	u32 colours = 0;
	u32 transform = 0;
	bool valid = false;
};
bool RecognizesVULightingProgram(std::span<const u8> micro, VULightingProgramWitness* witness = nullptr);
bool ValidateVULightingProgramWitness(std::span<const u8> micro, const VULightingProgramWitness& witness);
// Decode only after validating the current VU program.
LightingRig ReadVULightingValues(std::span<const u8> memory, bool normalized_colours);
LightingRig ReadVULighting(std::span<const u8> memory, std::span<const u8> micro, bool normalized_colours = false);

} // namespace remix_ps2::socom
