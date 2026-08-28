// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/Remix/RemixPaths.h"
#include "GS/Remix/RemixKnobs.h"
#include "GS/Remix/RemixRuntime.h"

#include "Config.h"
#include "Host.h"
#include "VMManager.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/StringUtil.h"

#include "fmt/format.h"

#include <algorithm>
#include <cctype>
#include <cwchar>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

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

			// Knobs whose variable was already set by the real environment when we started.
			//
			// These are never written, ever. The A/B harness passes knobs through the environment,
			// and a live re-apply that overwrote them would make every measurement silently read
			// whatever the GUI happened to hold instead of what the harness asked for. Recorded
			// once, before we set anything ourselves, because after that we could not tell our own
			// writes apart from the caller's.
			std::unordered_set<std::string> s_external_env;
			std::unordered_set<std::string> s_title_env;
			std::unordered_map<std::string, std::wstring> s_title_previous_env;

			// Bumped by apply_knob whenever a knob's environment value actually changes. The
			// backend caches its knob reads against this instead of re-reading the environment.
			u64 s_knob_generation = 0;

			bool externally_set(const knob& k)
			{
				return s_external_env.find(k.env) != s_external_env.end() ||
					s_title_env.find(k.env) != s_title_env.end();
			}

			std::string normalized_env_name(std::string_view name)
			{
				std::string normalized(name);
				std::transform(normalized.begin(), normalized.end(), normalized.begin(),
					[](unsigned char c) { return static_cast<char>(std::toupper(c)); });
				return normalized;
			}

			std::string short_env_name(std::string_view name)
			{
				constexpr std::string_view prefix = "PCSX2_REMIX_";
				std::string normalized = normalized_env_name(name);
				return (normalized.substr(0, prefix.size()) == prefix) ? normalized.substr(prefix.size()) : normalized;
			}

			void capture_external_env()
			{
				s_external_env.clear();

#ifdef _WIN32
				constexpr std::string_view prefix = "PCSX2_REMIX_";
				wchar_t* block = GetEnvironmentStringsW();
				if (!block)
					return;

				for (const wchar_t* entry = block; *entry; entry += std::wcslen(entry) + 1)
				{
					const wchar_t* equals = std::wcschr(entry, L'=');
					if (!equals)
						continue;

					const std::wstring_view wide_name(entry, static_cast<size_t>(equals - entry));
					const std::string name = normalized_env_name(StringUtil::WideStringToUTF8String(wide_name));
					if (name.substr(0, prefix.size()) != prefix)
						continue;

					s_external_env.insert(name.substr(prefix.size()));
				}

				FreeEnvironmentStringsW(block);
#else
				size_t count = 0;
				const knob* table = knobs(count);
				for (size_t i = 0; i < count; ++i)
				{
					if (!read_env((L"PCSX2_REMIX_" + widen(table[i].env)).c_str()).empty())
						s_external_env.insert(table[i].env);
				}
#endif
			}

			std::wstring env_name_for(const knob& k)
			{
				return L"PCSX2_REMIX_" + widen(k.env);
			}

			// Pushes one knob's setting into the environment.
			//
			// Absent from the .ini means "leave the backend default alone", so each is read with a
			// sentinel rather than with the backend's own default. Writing the backend default back
			// out would be indistinguishable from the user having chosen it, and would then outrank
			// a per-game .conf that wanted something else.
			void apply_knob(const knob& k)
			{
				if (externally_set(k))
					return;

				// Presence is tested as a STRING, never with an out-of-range numeric sentinel.
				//
				// This previously read the value as a float with -1e30 meaning "unset" and compared
				// the result back against -1e30. float cannot represent -1e30 exactly, so that
				// comparison was never true and every unset knob was treated as set and written out
				// as "-1e+30". The integer knobs then parsed to 0, which set TEXBUDGET to 0 (so
				// bind() returned null and nothing was ever textured) and SCANKICKS to 0 (so no VU
				// kicks were scanned and the camera never solved). The screen went black.
				//
				// A string read has no unrepresentable value and no in-band signalling: empty means
				// the key is absent, full stop.
				const std::string raw = Host::GetStringSettingValue(kSection, k.env, "");
				if (raw.empty())
					return;

				// Bools are stored by the Qt binder as true/false, everything else as a number.
				std::string text = raw;
				if (k.type == knob_type::Boolean)
					text = StringUtil::ToChars(StringUtil::FromChars<bool>(raw).value_or(false) ? 1 : 0);

				// Write only on a real change, and count the changes. A knob the settings page
				// presents as live is only live if the backend re-reads it, and the backend cannot
				// afford a GetEnvironmentVariableW per draw -- so it caches against this counter
				// and re-reads when it moves. Comparing here is what keeps that counter meaningful:
				// apply_live_knobs() runs every frame, so bumping unconditionally would invalidate
				// every cache every frame and put the env read back in the hot path.
				const std::wstring name = env_name_for(k);
				const std::wstring wide = widen(text);
				if (read_env(name.c_str()) == wide)
					return;

				SetEnvironmentVariableW(name.c_str(), wide.c_str());
				INFO_LOG("Remix: PCSX2_REMIX_{} = {} (PCSX2 GUI)", k.env, text);
				++s_knob_generation;
			}
		} // namespace

		u64 knob_generation()
		{
			return s_knob_generation;
		}

		void bump_knob_generation()
		{
			++s_knob_generation;
		}

		bool is_external_env(std::string_view name)
		{
			return s_external_env.find(short_env_name(name)) != s_external_env.end();
		}

		std::string env_value(std::string_view name)
		{
			return StringUtil::WideStringToUTF8String(read_env(widen(normalized_env_name(name)).c_str()));
		}

		const char* env_source(std::string_view name)
		{
			const std::string short_name = short_env_name(name);
			if (s_external_env.find(short_name) != s_external_env.end())
				return "external process";
			if (s_title_env.find(short_name) != s_title_env.end())
				return "per-game conf";
			return env_value(name).empty() ? "backend default" : "PCSX2 GUI";
		}

		bool apply_title_env(std::string_view name, std::string_view value)
		{
			const std::string short_name = short_env_name(name);
			if (s_external_env.find(short_name) != s_external_env.end())
				return false;

			const std::string normalized_name = normalized_env_name(name);
			const std::wstring wname = widen(normalized_name);
			const std::wstring wvalue = widen(std::string(value));
			const auto [previous_it, inserted] =
				s_title_previous_env.emplace(short_name, read_env(wname.c_str()));
			if (read_env(wname.c_str()) != wvalue)
			{
				if (!SetEnvironmentVariableW(wname.c_str(), wvalue.c_str()))
				{
					if (inserted)
						s_title_previous_env.erase(previous_it);
					return false;
				}
				++s_knob_generation;
			}

			s_title_env.insert(short_name);
			INFO_LOG("Remix: {} = {} (per-game conf)", name, value);
			return true;
		}

		bool clear_title_env()
		{
			bool changed = false;
			for (auto it = s_title_previous_env.begin(); it != s_title_previous_env.end(); )
			{
				const std::string& short_name = it->first;
				const std::wstring& previous = it->second;
				const std::wstring name = L"PCSX2_REMIX_" + widen(short_name);
				if (read_env(name.c_str()) == previous)
				{
					s_title_env.erase(short_name);
					it = s_title_previous_env.erase(it);
					continue;
				}

				if (!SetEnvironmentVariableW(name.c_str(), previous.empty() ? nullptr : previous.c_str()))
				{
					WARNING_LOG("Remix: failed to restore {} after per-game config; will retry", short_name);
					++it;
					continue;
				}

				changed = true;
				s_title_env.erase(short_name);
				it = s_title_previous_env.erase(it);
			}

			if (changed)
				++s_knob_generation;

			return s_title_previous_env.empty();
		}

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

		void apply_live_knobs()
		{
			// Latched knobs are skipped rather than written: the backend captured them at startup,
			// so writing them now would change what the settings page reports the backend is using
			// without changing anything the backend actually does.
			size_t count = 0;
			const knob* table = knobs(count);

			for (size_t i = 0; i < count; ++i)
			{
				if (!table[i].latched)
					apply_knob(table[i]);
			}
		}

		void apply_before_runtime_load()
		{
			// Record the caller's environment before we write a single variable, so the harness
			// keeps its precedence for the rest of the session.
			size_t count = 0;
			const knob* table = knobs(count);

			capture_external_env();

			// Every knob, latched or not. The latched ones are read through `static const` locals
			// in RemixSubmit, so they are captured the first time that code runs and this is the
			// only moment at which setting them does anything at all.
			for (size_t i = 0; i < count; ++i)
				apply_knob(table[i]);

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

			// The config layer, which is NOT part of the RtxFileSys table above and so was left
			// behind when the rest of the tree went per-game.
			//
			// The runtime opens its layers by RELATIVE name -- the log says "Found config file:
			// rtx.conf", not a path -- so they resolve against the working directory and every
			// title shared the one rtx.conf next to the exe. That is why a texture categorised in
			// the developer menu for one game was in scope for all of them, and why
			// RemixGames\<SERIAL>\ only ever collected a logs folder.
			//
			// DXVK_RTX_CONFIG_FILE is the runtime's own override for it (confirmed present in the
			// shipped d3d9.dll's string table alongside DXVK_CONFIG_FILE and DXVK_USE_CONF_FOR_EXE).
			// Pointed at the per-game folder, the tags a user saves land with the game they were
			// made for.
			//
			// NOT VERIFIED: whether the user.conf layer follows it. There is no
			// DXVK_USER_CONFIG_FILE in that string table, so user.conf may stay next to the exe --
			// in which case per-game covers the mod layer only and the user layer stays global.
			set_env_if_unset(L"DXVK_RTX_CONFIG_FILE", widen(Path::Combine(game_dir(), "rtx.conf")));

			INFO_LOG("Remix: per-game files for '{}' under '{}'", s_game_id_at_init, game_dir());
		}
	} // namespace paths
} // namespace remix_ps2
