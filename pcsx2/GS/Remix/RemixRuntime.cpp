// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Remix/RemixRuntime.h"

#include "Config.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"

#include <algorithm>
#include <cstdlib>
#include <cwchar>
#include <optional>
#include <vector>

// ---------------------------------------------------------------------------------------------
// Runtime provenance. The vendored header and the deployed runtime MUST come from the same
// release tag, always in the same commit -- the API's version gate only compares the minor
// number, so two builds of the same minor with a different interface layout would pass the
// check and then misroute every slot after the first divergence.
//
//   Fork          : RemixProjGroup/dxvk-remix ("Remix Plus", maintainer Kim2091)
//   Release tag   : remix-plus-1.5.1        (tag object f4173a9c8b94736363cb27c3bd228059780acbcf)
//   Tagged commit : 8afa36fdecc60e8d3ec57b360e5fd14158b39aea
//   API version   : 0.1000.0
//   remix_c.h     : blob 3f4acf5f476bf96d71bdd09354ef0602b0a8e239, 54643 bytes
//                   SHA-256 25296449789A483745E65E8A0110AACF134EA77E3AB608991DAA12A0FC9E7AE0
//                   (byte-identical to public/include/remix/remix_c.h at that tag)
//   Runtime asset : Remix_Plus_v1.5.1_x64_games_release.zip, deployed to <exe dir>\remix\
//
// Never update bin\remix\ without re-vendoring remix_c.h in the same commit.
// ---------------------------------------------------------------------------------------------

namespace remix_ps2
{
	namespace
	{
		constexpr u64 fnv_seed = 0xCBF29CE484222325ULL;
		constexpr u64 fnv_prime = 0x100000001B3ULL;

		// Widen a narrow path for the Remix loader, which is wchar_t only.
		std::wstring widen(const std::string& src)
		{
			if (src.empty())
				return {};

			const int needed = MultiByteToWideChar(CP_UTF8, 0, src.c_str(), static_cast<int>(src.size()), nullptr, 0);
			if (needed <= 0)
				return {};

			std::wstring result(static_cast<size_t>(needed), L'\0');
			MultiByteToWideChar(CP_UTF8, 0, src.c_str(), static_cast<int>(src.size()), result.data(), needed);
			return result;
		}

		// Narrow a UTF-16 path for logging only.
		std::string narrow(const std::wstring& src)
		{
			if (src.empty())
				return {};

			const int needed = WideCharToMultiByte(CP_UTF8, 0, src.c_str(), static_cast<int>(src.size()), nullptr, 0, nullptr, nullptr);
			if (needed <= 0)
				return {};

			std::string result(static_cast<size_t>(needed), '\0');
			WideCharToMultiByte(CP_UTF8, 0, src.c_str(), static_cast<int>(src.size()), result.data(), needed, nullptr, nullptr);
			return result;
		}

		// <exe dir>\remix\d3d9.dll, unless PCSX2_REMIX_DLL points elsewhere.
		// The runtime is deliberately NOT placed next to pcsx2-qt.exe: a file called d3d9.dll
		// there would be picked up by anything else in the process that resolves that name.
		std::wstring resolve_runtime_path()
		{
			if (std::wstring env = read_env(L"PCSX2_REMIX_DLL"); !env.empty())
				return env;

			return widen(Path::Combine(EmuFolders::AppRoot, "remix" FS_OSPATH_SEPARATOR_STR "d3d9.dll"));
		}

		// Identity of the DLL actually loaded, so a report can say which binary produced a run.
		void log_dll_identity(const std::wstring& path)
		{
			const std::string narrow_path = narrow(path);

			std::optional<std::vector<u8>> bytes = FileSystem::ReadBinaryFile(narrow_path.c_str());
			if (!bytes.has_value())
			{
				WARNING_LOG("Remix: runtime DLL '{}' could not be opened for fingerprinting", narrow_path);
				return;
			}

			u64 hash = fnv_seed;
			for (const u8 b : bytes.value())
			{
				hash ^= static_cast<u64>(b);
				hash *= fnv_prime;
			}

			INFO_LOG("Remix: runtime DLL '{}' size={} fnv1a={:016x}", narrow_path, bytes->size(), hash);
		}
	} // namespace

	std::wstring read_env(const wchar_t* name)
	{
		const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
		if (needed <= 1)
			return {};

		std::wstring result(static_cast<size_t>(needed) - 1, L'\0');
		const DWORD written = GetEnvironmentVariableW(name, result.data(), needed);
		result.resize(std::min<size_t>(written, result.size()));
		return result;
	}

	s64 read_env_int(const wchar_t* name, s64 fallback)
	{
		const std::wstring env = read_env(name);
		if (env.empty())
			return fallback;

		wchar_t* end = nullptr;
		const long long parsed = ::wcstoll(env.c_str(), &end, 10);
		if (end == env.c_str())
			return fallback;

		return static_cast<s64>(parsed);
	}

	runtime::~runtime()
	{
		shutdown();
	}

	bool runtime::initialize(HWND hwnd)
	{
		if (m_ok)
			return true;

		const std::wstring path = resolve_runtime_path();
		if (path.empty())
		{
			ERROR_LOG("Remix: could not resolve a runtime path");
			return false;
		}

		log_dll_identity(path);

		INFO_LOG("Remix: header API version {}.{}.{}, sizeof(remixapi_Interface)={} (+{} bytes slack)",
			u32{REMIXAPI_VERSION_MAJOR}, u32{REMIXAPI_VERSION_MINOR}, u32{REMIXAPI_VERSION_PATCH},
			sizeof(remixapi_Interface), interface_slack_bytes);

		const remixapi_ErrorCode load_status = remixapi_lib_loadRemixDllAndInitialize(path.c_str(), &m_storage.api, &m_dll);
		if (load_status != REMIXAPI_ERROR_CODE_SUCCESS)
		{
			// INCOMPATIBLE_VERSION here is the header/runtime lockstep check doing its job:
			// isVersionCompatible matches the minor exactly for 0.y.z, so both mismatch
			// directions fail loudly instead of silently misrouting interface slots.
			ERROR_LOG("Remix: remixapi_lib_loadRemixDllAndInitialize failed ({})", error_name(load_status));
			m_storage = {};
			m_dll = nullptr;
			return false;
		}

		remixapi_StartupInfo startup_info{};
		startup_info.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
		startup_info.pNext = nullptr;
		startup_info.hwnd = hwnd;
		startup_info.disableSrgbConversionForOutput = 0;
		startup_info.forceNoVkSwapchain = 0;
		startup_info.editorModeEnabled = 0;

		if (!m_storage.api.Startup)
		{
			ERROR_LOG("Remix: runtime exposes no Startup entry point");
			remixapi_lib_shutdownAndUnloadRemixDll(&m_storage.api, m_dll);
			m_storage = {};
			m_dll = nullptr;
			return false;
		}

		const u32 startup_status = guarded_startup(m_storage.api.Startup, &startup_info);
		if (startup_status != REMIXAPI_ERROR_CODE_SUCCESS)
		{
			ERROR_LOG("Remix: Startup failed ({})", error_name(startup_status));
			remixapi_lib_shutdownAndUnloadRemixDll(&m_storage.api, m_dll);
			m_storage = {};
			m_dll = nullptr;
			return false;
		}

		m_ok = true;

		// A stock (non-fork) runtime that somehow passed the version gate would leave these
		// slots at the zeroed value the storage was constructed with, so a null check is a
		// real detector and not just defensive noise.
		m_fork_features = check_fork_slots() && probe_create_texture();

		if (m_fork_features)
			INFO_LOG("Remix: fork features available (CreateTexture/CreateMaterial/DrawScreenOverlay)");
		else
			ERROR_LOG("Remix: fork features disabled -- running geometry-only (no textures, no UI compositor)");

		INFO_LOG("Remix: runtime initialized on HWND {}", static_cast<void*>(hwnd));
		return true;
	}

	bool runtime::check_fork_slots()
	{
		struct slot
		{
			const char* name;
			const void* fn;
		};

		const slot required[] =
			{
				{"CreateTexture", reinterpret_cast<const void*>(m_storage.api.CreateTexture)},
				{"DestroyTexture", reinterpret_cast<const void*>(m_storage.api.DestroyTexture)},
				{"CreateMaterial", reinterpret_cast<const void*>(m_storage.api.CreateMaterial)},
				{"DestroyMaterial", reinterpret_cast<const void*>(m_storage.api.DestroyMaterial)},
				{"DrawScreenOverlay", reinterpret_cast<const void*>(m_storage.api.DrawScreenOverlay)},
			};

		bool all_present = true;

		for (const slot& s : required)
		{
			if (!s.fn)
			{
				ERROR_LOG("Remix: runtime does not expose {} -- not a Remix Plus runtime?", s.name);
				all_present = false;
			}
		}

		return all_present;
	}

	bool runtime::probe_create_texture()
	{
		// 2x2 RGBA8, distinctive content. If a slot were misrouted the call would land in an
		// unrelated function; the SEH wrapper turns that into a loud degrade instead of a crash.
		// clang-format off
		static constexpr u8 probe_pixels[16] =
			{
				0xFF, 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF,
				0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
			};
		// clang-format on

		remixapi_TextureInfo info{};
		info.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
		info.pNext = nullptr;
		info.hash = 0x5043535832303031ull; // "PCSX2001"
		info.width = 2;
		info.height = 2;
		info.depth = 1;
		info.mipLevels = 1;
		info.format = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
		info.data = probe_pixels;
		info.dataSize = sizeof(probe_pixels);

		remixapi_TextureHandle handle = nullptr;
		const u32 status = guarded_create_texture(m_storage.api.CreateTexture, &info, &handle);

		if (status != REMIXAPI_ERROR_CODE_SUCCESS)
		{
			ERROR_LOG("Remix: CreateTexture probe failed ({})", error_name(status));
			return false;
		}

		INFO_LOG("Remix: CreateTexture probe SUCCESS (handle {})", static_cast<void*>(handle));

		if (handle)
			guarded_destroy_texture(m_storage.api.DestroyTexture, handle);

		return true;
	}

	void runtime::shutdown()
	{
		if (!m_dll)
		{
			m_storage = {};
			m_ok = false;
			m_fork_features = false;
			return;
		}

		remixapi_lib_shutdownAndUnloadRemixDll(&m_storage.api, m_dll);
		m_storage = {};
		m_dll = nullptr;
		m_ok = false;
		m_fork_features = false;
	}

	u32 guarded_startup(PFN_remixapi_Startup fn, const remixapi_StartupInfo* info)
	{
		if (!fn)
			return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;

		__try
		{
			return fn(info);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return error_code_faulted;
		}
	}

	u32 guarded_create_mesh(PFN_remixapi_CreateMesh fn, const remixapi_MeshInfo* info, remixapi_MeshHandle* out_handle)
	{
		if (!fn)
			return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;

		__try
		{
			return fn(info, out_handle);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return error_code_faulted;
		}
	}

	u32 guarded_destroy_mesh(PFN_remixapi_DestroyMesh fn, remixapi_MeshHandle handle)
	{
		if (!fn)
			return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;

		__try
		{
			return fn(handle);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return error_code_faulted;
		}
	}

	u32 guarded_draw_instance(PFN_remixapi_DrawInstance fn, const remixapi_InstanceInfo* info)
	{
		if (!fn)
			return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;

		__try
		{
			return fn(info);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return error_code_faulted;
		}
	}

	u32 guarded_setup_camera(PFN_remixapi_SetupCamera fn, const remixapi_CameraInfo* info)
	{
		if (!fn)
			return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;

		__try
		{
			return fn(info);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return error_code_faulted;
		}
	}

	u32 guarded_create_light(PFN_remixapi_CreateLight fn, const remixapi_LightInfo* info, remixapi_LightHandle* out_handle)
	{
		if (!fn)
			return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;

		__try
		{
			return fn(info, out_handle);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return error_code_faulted;
		}
	}

	u32 guarded_destroy_light(PFN_remixapi_DestroyLight fn, remixapi_LightHandle handle)
	{
		if (!fn)
			return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;

		__try
		{
			return fn(handle);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return error_code_faulted;
		}
	}

	u32 guarded_draw_light_instance(PFN_remixapi_DrawLightInstance fn, remixapi_LightHandle handle)
	{
		if (!fn)
			return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;

		__try
		{
			return fn(handle);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return error_code_faulted;
		}
	}

	u32 guarded_present(PFN_remixapi_Present fn, const remixapi_PresentInfo* info)
	{
		if (!fn)
			return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;

		__try
		{
			return fn(info);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return error_code_faulted;
		}
	}

	u32 guarded_create_material(PFN_remixapi_CreateMaterial fn, const remixapi_MaterialInfo* info, remixapi_MaterialHandle* out_handle)
	{
		if (!fn)
			return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;

		__try
		{
			return fn(info, out_handle);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return error_code_faulted;
		}
	}

	u32 guarded_destroy_material(PFN_remixapi_DestroyMaterial fn, remixapi_MaterialHandle handle)
	{
		if (!fn)
			return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;

		__try
		{
			return fn(handle);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return error_code_faulted;
		}
	}

	u32 guarded_create_texture(PFN_remixapi_CreateTexture fn, const remixapi_TextureInfo* info, remixapi_TextureHandle* out_handle)
	{
		if (!fn)
			return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;

		__try
		{
			return fn(info, out_handle);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return error_code_faulted;
		}
	}

	u32 guarded_destroy_texture(PFN_remixapi_DestroyTexture fn, remixapi_TextureHandle handle)
	{
		if (!fn)
			return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;

		__try
		{
			return fn(handle);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return error_code_faulted;
		}
	}

	u32 guarded_draw_screen_overlay(PFN_remixapi_DrawScreenOverlay fn, const void* pixels, u32 width, u32 height, remixapi_Format format, float opacity)
	{
		if (!fn)
			return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;

		__try
		{
			return fn(pixels, width, height, format, opacity);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return error_code_faulted;
		}
	}

	u32 guarded_set_config_variable(PFN_remixapi_SetConfigVariable fn, const char* key, const char* value)
	{
		if (!fn)
			return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;

		__try
		{
			return fn(key, value);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return error_code_faulted;
		}
	}

	const char* error_name(u32 code)
	{
		switch (code)
		{
				// clang-format off
			case REMIXAPI_ERROR_CODE_SUCCESS: return "SUCCESS";
			case REMIXAPI_ERROR_CODE_GENERAL_FAILURE: return "GENERAL_FAILURE";
			case REMIXAPI_ERROR_CODE_LOAD_LIBRARY_FAILURE: return "LOAD_LIBRARY_FAILURE";
			case REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS: return "INVALID_ARGUMENTS";
			case REMIXAPI_ERROR_CODE_GET_PROC_ADDRESS_FAILURE: return "GET_PROC_ADDRESS_FAILURE";
			case REMIXAPI_ERROR_CODE_ALREADY_EXISTS: return "ALREADY_EXISTS";
			case REMIXAPI_ERROR_CODE_REGISTERING_NON_REMIX_D3D9_DEVICE: return "REGISTERING_NON_REMIX_D3D9_DEVICE";
			case REMIXAPI_ERROR_CODE_REMIX_DEVICE_WAS_NOT_REGISTERED: return "REMIX_DEVICE_WAS_NOT_REGISTERED";
			case REMIXAPI_ERROR_CODE_INCOMPATIBLE_VERSION: return "INCOMPATIBLE_VERSION";
			case REMIXAPI_ERROR_CODE_SET_DLL_DIRECTORY_FAILURE: return "SET_DLL_DIRECTORY_FAILURE";
			case REMIXAPI_ERROR_CODE_GET_FULL_PATH_NAME_FAILURE: return "GET_FULL_PATH_NAME_FAILURE";
			case REMIXAPI_ERROR_CODE_NOT_INITIALIZED: return "NOT_INITIALIZED";
			case REMIXAPI_ERROR_CODE_HRESULT_NO_REQUIRED_GPU_FEATURES: return "HRESULT_NO_REQUIRED_GPU_FEATURES";
			case REMIXAPI_ERROR_CODE_HRESULT_DRIVER_VERSION_BELOW_MINIMUM: return "HRESULT_DRIVER_VERSION_BELOW_MINIMUM";
			case REMIXAPI_ERROR_CODE_HRESULT_DXVK_INSTANCE_EXTENSION_FAIL: return "HRESULT_DXVK_INSTANCE_EXTENSION_FAIL";
			case REMIXAPI_ERROR_CODE_HRESULT_VK_CREATE_INSTANCE_FAIL: return "HRESULT_VK_CREATE_INSTANCE_FAIL";
			case REMIXAPI_ERROR_CODE_HRESULT_VK_CREATE_DEVICE_FAIL: return "HRESULT_VK_CREATE_DEVICE_FAIL";
			case REMIXAPI_ERROR_CODE_HRESULT_GRAPHICS_QUEUE_FAMILY_MISSING: return "HRESULT_GRAPHICS_QUEUE_FAMILY_MISSING";
			case error_code_faulted: return "FAULTED (structured exception inside the runtime)";
			default: return "UNKNOWN";
				// clang-format on
		}
	}

	float hardcoded_far_plane()
	{
		static const float value = []() -> float {
			if (const std::wstring env = read_env(L"PCSX2_REMIX_FARPLANE"); !env.empty())
			{
				const float parsed = static_cast<float>(::_wtof(env.c_str()));
				if (parsed > 1.f)
					return parsed;
			}

			return 1000.f;
		}();

		return value;
	}
} // namespace remix_ps2
