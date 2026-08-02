// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

// Must precede remix_c.h: the vendored header includes <windows.h> raw, and without NOMINMAX
// the min/max macros poison every GS header (GSVector.h, GSTextureCache.h) included after it.
// RedtapeWindows.h is the in-tree wrapper that defines NOMINMAX/WIN32_LEAN_AND_MEAN first, so
// remix_c.h's own <windows.h> then hits the include guard and is a no-op.
#include "common/RedtapeWindows.h"

#include "GS/Remix/remix_c.h"

#include <cstddef>
#include <string>

namespace remix_ps2
{
	// Returned by the guarded wrappers when the runtime raised a structured exception.
	// Chosen outside the remixapi_ErrorCode range so it can never collide with a real code.
	inline constexpr u32 error_code_faulted = 0x7FFF0001u;

	// The runtime writes sizeof(its own remixapi_Interface) bytes into the caller's buffer.
	// A future runtime whose interface grew but whose minor version still reads 1000 would
	// therefore run off the end of a bare remixapi_Interface. Embedding it in a struct with
	// slack turns that from "corrupts the stack/heap next to it" into "corrupts padding we own".
	inline constexpr size_t interface_slack_bytes = 512;

	struct interface_storage
	{
		remixapi_Interface api{};
		u8 slack[interface_slack_bytes]{};
	};

	// Owns the dxvk-remix module and its interface table.
	// Nothing here throws: a missing or broken runtime degrades the renderer to a no-op.
	class runtime
	{
	public:
		runtime() = default;
		~runtime();

		runtime(const runtime&) = delete;
		runtime& operator=(const runtime&) = delete;

		// Loads the runtime DLL and calls Startup() against 'hwnd'.
		bool initialize(HWND hwnd);
		void shutdown();

		bool ok() const { return m_ok; }
		const remixapi_Interface& api() const { return m_storage.api; }

		// True only when every Remix Plus fork slot resolved AND the CreateTexture probe
		// succeeded. Everything beyond raw geometry is gated on this; false means the
		// backend runs in geometry-only mode.
		bool fork_features() const { return m_fork_features; }

	private:
		bool check_fork_slots();
		bool probe_create_texture();

		interface_storage m_storage{};
		HMODULE m_dll = nullptr;
		bool m_ok = false;
		bool m_fork_features = false;
	};

	// SEH-guarded leaf calls. POD parameters only: __try/__except cannot coexist with
	// objects that require unwinding in the same function (C2712).
	u32 guarded_startup(PFN_remixapi_Startup fn, const remixapi_StartupInfo* info);
	u32 guarded_create_mesh(PFN_remixapi_CreateMesh fn, const remixapi_MeshInfo* info, remixapi_MeshHandle* out_handle);
	u32 guarded_destroy_mesh(PFN_remixapi_DestroyMesh fn, remixapi_MeshHandle handle);
	u32 guarded_draw_instance(PFN_remixapi_DrawInstance fn, const remixapi_InstanceInfo* info);
	u32 guarded_setup_camera(PFN_remixapi_SetupCamera fn, const remixapi_CameraInfo* info);
	u32 guarded_create_light(PFN_remixapi_CreateLight fn, const remixapi_LightInfo* info, remixapi_LightHandle* out_handle);
	u32 guarded_destroy_light(PFN_remixapi_DestroyLight fn, remixapi_LightHandle handle);
	u32 guarded_draw_light_instance(PFN_remixapi_DrawLightInstance fn, remixapi_LightHandle handle);
	u32 guarded_present(PFN_remixapi_Present fn, const remixapi_PresentInfo* info);
	u32 guarded_create_material(PFN_remixapi_CreateMaterial fn, const remixapi_MaterialInfo* info, remixapi_MaterialHandle* out_handle);
	u32 guarded_destroy_material(PFN_remixapi_DestroyMaterial fn, remixapi_MaterialHandle handle);
	u32 guarded_create_texture(PFN_remixapi_CreateTexture fn, const remixapi_TextureInfo* info, remixapi_TextureHandle* out_handle);
	u32 guarded_destroy_texture(PFN_remixapi_DestroyTexture fn, remixapi_TextureHandle handle);
	u32 guarded_draw_screen_overlay(PFN_remixapi_DrawScreenOverlay fn, const void* pixels, u32 width, u32 height, remixapi_Format format, float opacity);
	u32 guarded_set_config_variable(PFN_remixapi_SetConfigVariable fn, const char* key, const char* value);

	// Readable name for a remixapi_ErrorCode or for error_code_faulted.
	const char* error_name(u32 code);

	// Far plane used by the hardcoded camera. Overridable at runtime through
	// PCSX2_REMIX_FARPLANE so a unit-scale sweep needs no rebuild.
	float hardcoded_far_plane();

	// Reads an environment variable as UTF-16. Empty if unset or empty.
	// Shared by the other Remix translation units (all PCSX2_REMIX_* toggles).
	std::wstring read_env(const wchar_t* name);

	// Reads a PCSX2_REMIX_* environment variable as an integer, 'fallback' when unset or
	// unparseable.
	s64 read_env_int(const wchar_t* name, s64 fallback);
} // namespace remix_ps2
