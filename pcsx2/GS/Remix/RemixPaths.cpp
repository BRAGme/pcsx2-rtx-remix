// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Remix/RemixPaths.h"
#include "GS/Remix/RemixRuntime.h"

#include "Config.h"
#include "Host.h"
#include "VMManager.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"

#include "fmt/format.h"

#include <string>

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#endif

namespace remix_ps2
{
	namespace paths
	{
		namespace
		{
			// The section the Qt settings page writes. One section, so the whole feature can be
			// read (and reset) as a unit.
			constexpr const char* kSection = "Remix";

			// NOT "Remix". The runtime already lives in <AppRoot>\remix\d3d9.dll, and in a
			// portable install DataRoot == AppRoot -- so on Windows, where paths are
			// case-insensitive, a folder named "Remix" resolves to that same directory and the
			// per-game trees get created in among the runtime's own DLLs. Measured: the first run
			// of this code put SLUS-20883 next to d3d9.dll.
			constexpr const char* kDefaultFolder = "RemixGames";

			std::string s_game_id_at_init;

			std::wstring widen(const std::string& value)
			{
#ifdef _WIN32
				if (value.empty())
					return {};

				const int needed = MultiByteToWideChar(CP_UTF8, 0, value.c_str(),
					static_cast<int>(value.size()), nullptr, 0);
				if (needed <= 0)
					return {};

				std::wstring out(static_cast<size_t>(needed), L'\0');
				MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
					out.data(), needed);
				return out;
#else
				return std::wstring(value.begin(), value.end());
#endif
			}

			// Sets an environment variable only when it is not already set.
			//
			// The precedence is deliberate and load-bearing: the A/B harness passes knobs through
			// the environment, and silently overwriting them from the .ini would make every
			// measurement read whatever the GUI happened to hold. Same rule refresh_game_config
			// uses for the per-game .conf files.
			void set_env_if_unset(const wchar_t* name, const std::wstring& value)
			{
				if (value.empty())
					return;

#ifdef _WIN32
				if (!read_env(name).empty())
					return;

				SetEnvironmentVariableW(name, value.c_str());
#endif
			}

			// Bridges one float setting onto a PCSX2_REMIX_* knob. Absent from the .ini means
			// "leave the backend default alone" -- which is why every one of these is read with a
			// sentinel rather than with the backend's own default value. Writing the backend
			// default back out would be indistinguishable from the user having chosen it, and
			// would then outrank a per-game .conf that wanted something else.
			void bridge_float(const char* key, const wchar_t* env_name)
			{
				constexpr float kUnset = -1e30f;

				const float value = Host::GetFloatSettingValue(kSection, key, kUnset);
				if (value == kUnset)
					return;

				set_env_if_unset(env_name, widen(fmt::format("{:g}", value)));
			}

			void bridge_int(const char* key, const wchar_t* env_name)
			{
				constexpr int kUnset = INT32_MIN;

				const int value = Host::GetIntSettingValue(kSection, key, kUnset);
				if (value == kUnset)
					return;

				set_env_if_unset(env_name, widen(fmt::format("{}", value)));
			}
		} // namespace

		bool per_game_enabled()
		{
			return Host::GetBoolSettingValue(kSection, "EnablePerGameFiles", true);
		}

		std::string remix_root()
		{
			std::string configured(Host::GetStringSettingValue(kSection, "FolderPath", ""));
			if (configured.empty())
				return Path::Combine(EmuFolders::DataRoot, kDefaultFolder);

			// A relative path in the .ini is resolved against the data root, matching how
			// PCSX2's own folder settings behave.
			if (!Path::IsAbsolute(configured))
				return Path::Combine(EmuFolders::DataRoot, configured);

			return configured;
		}

		std::string game_id()
		{
			const std::string serial = VMManager::GetDiscSerial();
			if (serial.empty())
				return {};

			return Path::SanitizeFileName(serial);
		}

		std::string game_dir()
		{
			if (!per_game_enabled())
				return {};

			const std::string id = game_id();
			if (id.empty())
				return {};

			return Path::Combine(remix_root(), id);
		}

		std::string game_subdir(const char* leaf)
		{
			const std::string dir = game_dir();
			if (dir.empty())
				return {};

			return Path::Combine(dir, leaf);
		}

		bool ensure_game_dirs()
		{
			const std::string dir = game_dir();
			if (dir.empty())
				return false;

			if (!FileSystem::EnsureDirectoryExists(dir.c_str(), true))
			{
				ERROR_LOG("Remix: could not create per-game directory '{}'", dir);
				return false;
			}

			// Missing children are not fatal on their own -- Remix creates what it needs under a
			// root that exists -- but making them up front is what lets the GUI's "open folder"
			// button land somewhere useful before the game has ever written anything.
			for (const char* leaf : {"mods", "captures", "logs"})
			{
				const std::string child = Path::Combine(dir, leaf);
				if (!FileSystem::EnsureDirectoryExists(child.c_str(), true))
					WARNING_LOG("Remix: could not create '{}'", child);
			}

			return true;
		}

		std::wstring runtime_dll()
		{
			if (std::wstring env = read_env(L"PCSX2_REMIX_DLL"); !env.empty())
				return env;

			if (std::string configured(Host::GetStringSettingValue(kSection, "RuntimePath", ""));
				!configured.empty())
			{
				return widen(configured);
			}

			return widen(Path::Combine(EmuFolders::AppRoot, "remix" FS_OSPATH_SEPARATOR_STR "d3d9.dll"));
		}

		const std::string& game_id_at_init()
		{
			return s_game_id_at_init;
		}

		void apply_before_runtime_load()
		{
			// Value knobs first: these are read through `static const` locals in RemixSubmit, so
			// they latch on first use and there is no later point at which setting them works.
			bridge_float("WorldScale", L"PCSX2_REMIX_CAMSCALE");
			bridge_float("LightBrightness", L"PCSX2_REMIX_KEY");
			bridge_float("LightAngle", L"PCSX2_REMIX_KEYANGLE");
			bridge_float("AmbientBrightness", L"PCSX2_REMIX_AMBIENT");
			bridge_int("LightMode", L"PCSX2_REMIX_LIGHTMODE");

			s_game_id_at_init = game_id();

			if (!per_game_enabled())
			{
				INFO_LOG("Remix: per-game files disabled, using the shared rtx-remix tree");
				return;
			}

			if (s_game_id_at_init.empty())
			{
				WARNING_LOG("Remix: no disc serial at runtime init -- per-game files inactive for "
							"this session");
				return;
			}

			if (!ensure_game_dirs())
				return;

			// RtxFileSys reads these once, while the runtime DLL initialises
			// (util_filesys.h: the PathSpec table names DEFAULT_MODS_DIR / DXVK_CAPTURE_PATH /
			// DXVK_LOG_PATH). After the DLL is loaded they have no effect whatsoever.
			set_env_if_unset(L"DEFAULT_MODS_DIR", widen(game_subdir("mods")));
			set_env_if_unset(L"DXVK_CAPTURE_PATH", widen(game_subdir("captures")));
			set_env_if_unset(L"DXVK_LOG_PATH", widen(game_subdir("logs")));

			INFO_LOG("Remix: per-game files for '{}' under '{}'", s_game_id_at_init, game_dir());
		}
	} // namespace paths
} // namespace remix_ps2
