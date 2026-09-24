// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "RemixSocomLighting.h"
#include "RemixSocomCamera.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace remix_ps2::socom
{
namespace
{
constexpr u32 ram_size = 0x02000000;

bool readable(std::span<const u8> memory, u32 address, u32 size)
{
	return address != 0 && address <= memory.size() && size <= memory.size() - address;
}

u32 word(std::span<const u8> memory, u32 address)
{
	u32 value;
	std::memcpy(&value, memory.data() + address, sizeof(value));
	return value;
}

bool text_is(std::span<const u8> memory, u32 address, std::string_view text)
{
	return readable(memory, address, static_cast<u32>(text.size() + 1)) &&
		std::memcmp(memory.data() + address, text.data(), text.size()) == 0 && memory[address + text.size()] == 0;
}

bool class_is(std::span<const u8> memory, u32 object, u32 vtable_offset, u32 vtable,
	u32 type_info, u32 name, std::string_view text)
{
	return readable(memory, object, vtable_offset + 4) && word(memory, object + vtable_offset) == vtable &&
		readable(memory, vtable, 4) && word(memory, vtable) == type_info &&
		readable(memory, type_info, 4) && word(memory, type_info) == name && text_is(memory, name, text);
}

bool lighting_record(std::span<const u8> memory, u32 address, LightingRig& rig)
{
	if (!readable(memory, address, 0x70) || (address & 15) != 0)
		return false;
	float values[7][4];
	std::memcpy(values, memory.data() + address, sizeof(values));
	for (const auto& vector : values)
	{
		for (const float value : vector)
			if (!std::isfinite(value))
				return false;
		if (std::abs(vector[3]) > 1e-6f)
			return false;
	}
	for (u32 i = 3; i < 7; ++i)
		for (u32 j = 0; j < 3; ++j)
			if (values[i][j] < 0.f || values[i][j] > 1.f)
				return false;
	for (u32 i = 0; i < 3; ++i)
	{
		const float length = std::sqrt(values[i][0] * values[i][0] + values[i][1] * values[i][1] + values[i][2] * values[i][2]);
		const bool enabled = values[i + 3][0] > 0.f || values[i + 3][1] > 0.f || values[i + 3][2] > 0.f;
		if (enabled && (!std::isfinite(length) || length < 1e-6f))
			return false;
		for (u32 j = 0; j < 3; ++j)
		{
			rig.directions[i][j] = enabled ? values[i][j] / length : 0.f;
			rig.colours[i][j] = values[i + 3][j];
		}
	}
	std::memcpy(rig.ambient, values[6], sizeof(rig.ambient));
	return true;
}

bool bounded_name(std::span<const u8> memory, u32 address, char (&name)[64])
{
	if (!readable(memory, address, sizeof(name)))
		return false;
	for (u32 i = 0; i < sizeof(name); ++i)
	{
		const char c = static_cast<char>(memory[address + i]);
		if (c == 0)
			return i != 0;
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
				(c >= '0' && c <= '9') || c == '_' || c == '/' || c == '\\' || c == '.'))
			return false;
		name[i] = (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
	}
	return false;
}

bool s1_mission(std::string_view name)
{
	constexpr std::string_view names[] = {"m11", "m12", "m13", "m15", "m16", "m17", "m19", "m21", "m5", "m6", "m8", "m9",
		"mp1", "mp10", "mp11", "mp12", "mp2", "mp5", "mp6", "mp7", "mp8", "mp9"};
	return std::find(std::begin(names), std::end(names), name) != std::end(names);
}

bool ca_ui_lighting(std::span<const u8> memory, u32 address = 0x00743CD0)
{
	// The active UI world owns this rig even while CMission retains a previous mission.
	constexpr float ui[7][4] = {{1.f, -2.4f, 3.4f, 0.f}, {-5.f, -1.f, -0.5f, 0.f}, {5.f, 1.f, 0.5f, 0.f},
		{64.f / 255.f, 64.f / 255.f, 64.f / 255.f, 0.f}, {64.f / 255.f, 64.f / 255.f, 96.f / 255.f, 0.f},
		{32.f / 255.f, 32.f / 255.f, 44.f / 255.f, 0.f}, {64.f / 255.f, 64.f / 255.f, 64.f / 255.f, 0.f}};
	float active[7][4];
	std::memcpy(active, memory.data() + address, sizeof(active));
	for (u32 i = 0; i < 7; ++i)
		for (u32 j = 0; j < 4; ++j)
			if (std::abs(active[i][j] - ui[i][j]) > 1e-6f)
				return false;
	return true;
}

LightingRig read_s1(std::span<const u8> memory)
{
	LightingRig rig;
	const u32 world = word(memory, 0x0048D848);
	if (!class_is(memory, world, 0x60, 0x00489910, 0x00467B48, 0x00467B20, "zdb::CWorld") ||
		!readable(memory, world, 0x674) || !bounded_name(memory, word(memory, world + 0x90), rig.world_name) ||
		!s1_mission(rig.world_name))
		return {};
	const u32 camera = word(memory, world + 0xC0);
	if (camera != word(memory, 0x0048E4B8) ||
		!class_is(memory, camera, 0x60, 0x004893C0, 0x004645C0, 0x00464520, "zdb::CCamera") ||
		!lighting_record(memory, world + 0x600, rig) || world != word(memory, 0x0048D848))
		return {};
	rig.valid = true;
	rig.evidence = LightingEvidence::NativeCodeLayout; // needs proper testing in a live S1 mission.
	rig.world_identity = world;
	rig.camera_address = camera;
	return rig;
}

LightingRig read_ca(std::span<const u8> memory)
{
	LightingRig rig;
	constexpr u32 mission = 0x00709890;
	if (!class_is(memory, mission, 0, 0x006E55E0, 0x006B3B80, 0x006B3B70, "CMission") ||
		word(memory, 0x00743CC0) != 0x00743CD0 || !bounded_name(memory, mission + 0x78, rig.world_name))
		return {};
	const u32 mission_data = word(memory, mission + 0x18);
	if (!readable(memory, mission_data, 0x10) || word(memory, mission_data + 0xC) != word(memory, mission + 0x20))
		return {};
	const u32 camera = word(memory, 0x00743E10);
	if (ca_ui_lighting(memory))
	{
		rig.menu = true;
		rig.evidence = LightingEvidence::NativeSaveState;
		return rig;
	}
	socom_camera::Snapshot snapshot;
	if (!class_is(memory, camera, 0, 0x006E51E0, 0x006B1F58, 0x006B1F20, "zdb::CCamera") ||
		!readable(memory, camera, 0x670) || word(memory, camera + 0x1D0) != camera + 0x510 ||
		!socom_camera::ReadSnapshot(memory.data(), memory.size(), socom_camera::combined_assault_crc, snapshot) ||
		!socom_camera::IsRaster(snapshot.world_to_screen) || !lighting_record(memory, 0x00743CD0, rig))
		return {};
	rig.valid = true;
	rig.evidence = LightingEvidence::NativeSaveState;
	rig.world_identity = word(memory, mission + 0x20);
	rig.camera_address = camera;
	if (mission_data != word(memory, mission + 0x18) || camera != word(memory, 0x00743E10))
		return {};
	return rig;
}

} // namespace

LightingRig ReadLighting(std::span<const u8> memory, std::string_view serial, u32 crc)
{
	if (memory.size() != ram_size)
		return {};
	if (serial == "SCUS-97134" && crc == 0x6F4056DB)
		return read_s1(memory);
	if (serial == "SCUS-97545" && crc == 0xD7CFDCCF)
		return read_ca(memory);
	if (serial == "SCUS-97474" && crc == 0x75ED4282)
	{
		const auto mission = ReadMission(memory, serial, crc);
		LightingRig rig;
		rig.menu = mission.menu;
		if (!mission.valid || !lighting_record(memory, 0x0069BBD0, rig))
			return rig;
		rig.valid = true;
		rig.evidence = LightingEvidence::NativeSaveState;
		rig.world_identity = mission.world_identity;
		rig.camera_address = word(memory, 0x0069BD10);
		std::memcpy(rig.world_name, mission.world_name, sizeof(rig.world_name));
		return rig;
	}
	return {};
}

MissionSnapshot ReadMission(std::span<const u8> memory, std::string_view serial, u32 crc)
{
	if (memory.size() != ram_size)
		return {};
	const bool ca = serial == "SCUS-97545" && crc == 0xD7CFDCCF;
	const bool s3 = serial == "SCUS-97474" && crc == 0x75ED4282;
	if (!ca && !s3)
		return {};
	const u32 object = ca ? 0x00709890 : 0x00671FC0;
	if (!class_is(memory, object, 0, ca ? 0x006E55E0 : 0x0064EDB0,
		ca ? 0x006B3B80 : 0x0061C800, ca ? 0x006B3B70 : 0x0061C7F0, "CMission"))
		return {};
	const u32 lights = ca ? 0x00743CD0 : 0x0069BBD0;
	if (word(memory, ca ? 0x00743CC0 : 0x0069BBC0) != lights)
		return {};
	MissionSnapshot result;
	if (ca_ui_lighting(memory, lights))
	{
		result.menu = true;
		return result;
	}
	const u32 data_offset = ca ? 0x18 : 0x04;
	const u32 data = word(memory, object + data_offset);
	if (!readable(memory, data, ca ? 0x10 : 0x40) ||
		!bounded_name(memory, object + (ca ? 0x78 : 0x5C), result.world_name) ||
		!bounded_name(memory, object + (ca ? 0x38 : 0x1C), result.mission_name))
		return {};
	result.world_identity = word(memory, object + (ca ? 0x20 : 0x10));
	if (!result.world_identity || (ca && word(memory, data + 0xC) != result.world_identity))
		return {};
	if (s3)
	{
		char linked_name[64] = {};
		if (word(memory, data + 0x04) != word(memory, data + 0x3C) ||
			!bounded_name(memory, word(memory, data + 0x04), linked_name) ||
			std::strcmp(linked_name, result.world_name) != 0 ||
			word(memory, object + 0x0C) != result.world_identity)
			return {};
	}
	const u32 camera_global = ca ? 0x00743E10 : 0x0069BD10;
	const u32 camera = word(memory, camera_global);
	if (!class_is(memory, camera, 0, ca ? 0x006E51E0 : 0x0064E9E0,
		ca ? 0x006B1F58 : 0x00619708, ca ? 0x006B1F20 : 0x006196D0, "zdb::CCamera") ||
		!readable(memory, camera, 0x670) || word(memory, camera + 0x1D0) != camera + 0x510)
		return {};
	mat4 raster;
	std::memcpy(&raster, memory.data() + camera + 0x530, sizeof(raster));
	if (!socom_camera::IsRaster(raster) || data != word(memory, object + data_offset) ||
		camera != word(memory, camera_global) || result.world_identity != word(memory, object + (ca ? 0x20 : 0x10)))
		return {};
	result.valid = true;
	return result;
}

} // namespace remix_ps2::socom

namespace remix_ps2::socom
{
namespace
{
// Shared S1/CA instructions: absolute world normal rows and native RGB rows.
constexpr u32 normals[] = {0x01F50010, 0x01F60011, 0x01F70012, 0x01F80013};
constexpr u32 colours[] = {0x01E90014, 0x01EA0015, 0x01EB0016, 0x01EC0017};
// MULA.x / MADDA.y / MADDA.z / MADD.w transform VF21..24 by object VF17.
constexpr u32 transform[] = {0x01F1A9BC, 0x01F1B0BD, 0x01F1B8BE, 0x01F1C14B};

bool lighting_instructions(std::span<const u8> micro, u32 address, const u32 (&instructions)[4], bool upper)
{
	for (u32 i = 0; i < 4; ++i)
	{
		u32 instruction = word(micro, address + i * 8 + (upper ? 4 : 0));
		if (upper)
			instruction &= 0x07FFFFFF; // Ignore I/E/M/D/T scheduling bits.
		if (instruction != instructions[i])
			return false;
	}
	return true;
}
}

bool ValidateVULightingProgramWitness(std::span<const u8> micro, const VULightingProgramWitness& witness)
{
	if (micro.size() != 0x4000 || !micro.data() || !witness.valid || (witness.normals & 7) != 0 ||
		witness.normals > micro.size() - 32 || (witness.colours & 7) != 0 || (witness.transform & 7) != 0)
		return false;
	const u32 limit = std::min<u32>(static_cast<u32>(micro.size()) - 32, witness.normals + 512);
	if (witness.colours < witness.normals + 32 || witness.transform < witness.normals + 32 ||
		witness.colours > limit || witness.transform > limit)
		return false;
	return lighting_instructions(micro, witness.normals, normals, false) &&
		lighting_instructions(micro, witness.colours, colours, false) &&
		lighting_instructions(micro, witness.transform, transform, true);
}

bool RecognizesVULightingProgram(std::span<const u8> micro, VULightingProgramWitness* witness)
{
	if (witness)
		*witness = {};
	if (micro.size() != 0x4000)
		return false;
	for (u32 start = 0; start + 32 <= micro.size(); start += 8)
	{
		if (!lighting_instructions(micro, start, normals, false))
			continue;
		bool found_colours = false;
		bool found_transform = false;
		u32 colour_offset = 0;
		u32 transform_offset = 0;
		const u32 limit = std::min<u32>(static_cast<u32>(micro.size()) - 32, start + 512);
		for (u32 next = start + 32; next <= limit; next += 8)
		{
			if (lighting_instructions(micro, next, colours, false))
			{
				found_colours = true;
				colour_offset = next;
			}
			if (lighting_instructions(micro, next, transform, true))
			{
				found_transform = true;
				transform_offset = next;
			}
		}
		if (found_colours && found_transform)
		{
			if (witness)
				*witness = {start, colour_offset, transform_offset, true};
			return true;
		}
	}
	return false;
}

LightingRig ReadVULighting(std::span<const u8> memory, std::span<const u8> micro, bool normalized_colours)
{
	if (memory.size() != 0x4000 || !RecognizesVULightingProgram(micro))
		return {};
	return ReadVULightingValues(memory, normalized_colours);
}

LightingRig ReadVULightingValues(std::span<const u8> memory, bool normalized_colours)
{
	if (memory.size() != 0x4000)
		return {};
	float values[8][4];
	std::memcpy(values, memory.data() + 16 * 16, sizeof(values));
	const float colour_limit = normalized_colours ? 1.001f : 255.001f;
	const float colour_scale = normalized_colours ? 1.f : 1.f / 255.f;
	for (const auto& vector : values)
		for (float value : vector)
			if (!std::isfinite(value))
				return {};
	for (u32 i = 0; i < 3; ++i)
		if (std::abs(values[i][3]) > 1e-6f || std::abs(values[3][i]) > 1e-6f)
			return {};
	if (std::abs(values[3][3] - 1.f) > 1e-6f)
		return {};
	for (u32 i = 4; i < 8; ++i)
	{
		if (std::abs(values[i][3]) > 1e-6f)
			return {}; // Verified native rig W=0; CA prelit/HUD ambient W=255.
		for (u32 j = 0; j < 3; ++j)
			if (values[i][j] < 0.f || values[i][j] > colour_limit)
				return {};
	}
	LightingRig rig;
	bool any_direction = false;
	for (u32 i = 0; i < 3; ++i)
	{
		const bool enabled = values[i + 4][0] > 0.f || values[i + 4][1] > 0.f || values[i + 4][2] > 0.f;
		const float length = std::sqrt(values[0][i] * values[0][i] + values[1][i] * values[1][i] + values[2][i] * values[2][i]);
		if (enabled && (!std::isfinite(length) || std::abs(length - 1.f) > 0.02f))
			return {};
		any_direction |= enabled;
		for (u32 j = 0; j < 3; ++j)
		{
			// q16 is world-space already; only q30..32 hold the object result.
			rig.directions[i][j] = enabled ? -values[j][i] / length : 0.f;
			rig.colours[i][j] = values[i + 4][j] * colour_scale;
		}
	}
	for (u32 j = 0; j < 3; ++j)
		rig.ambient[j] = values[7][j] * colour_scale;
	if (!any_direction && rig.ambient[0] == 0.f && rig.ambient[1] == 0.f && rig.ambient[2] == 0.f)
		return {};
	// Authored UI rigs. Native CMission can retain a previous mission in menus.
	constexpr float ui_dirs[][3][3] = {{{.47f, -.75f, .47f}, {}, {}},
		{{1.f, -2.4f, 3.4f}, {-5.f, -1.f, -.5f}, {5.f, 1.f, .5f}}};
	constexpr float ui_colours[][3][3] = {{{.8f, .8f, .8f}, {}, {}},
		{{64.f/255.f, 64.f/255.f, 64.f/255.f}, {64.f/255.f, 64.f/255.f, 96.f/255.f}, {32.f/255.f, 32.f/255.f, 44.f/255.f}}};
	constexpr float ui_ambient[] = {.56f, 142.f/255.f, 64.f/255.f};
	for (u32 candidate = 0; candidate < 3; ++candidate)
	{
		const u32 set = candidate == 2 ? 1 : 0;
		bool matches = true;
		for (u32 i = 0; i < 3; ++i)
		{
			const float length = std::sqrt(ui_dirs[set][i][0] * ui_dirs[set][i][0] + ui_dirs[set][i][1] * ui_dirs[set][i][1] + ui_dirs[set][i][2] * ui_dirs[set][i][2]);
			for (u32 j = 0; j < 3; ++j)
			{
				matches &= std::abs(rig.directions[i][j] - (length > 0.f ? ui_dirs[set][i][j] / length : 0.f)) < 1e-5f;
				matches &= std::abs(rig.colours[i][j] - ui_colours[set][i][j]) < 1e-5f;
			}
			matches &= std::abs(rig.ambient[i] - ui_ambient[candidate]) < 1e-5f;
		}
		if (matches)
		{
			rig.menu = true;
			rig.evidence = LightingEvidence::NativeVUProgram;
			return rig;
		}
	}
	rig.valid = true;
	rig.evidence = LightingEvidence::NativeVUProgram;
	std::memcpy(rig.world_name, "native VU mission rig", sizeof("native VU mission rig"));
	return rig;
}
} // namespace remix_ps2::socom
