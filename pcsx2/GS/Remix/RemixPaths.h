// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <string>
#include <string_view>

// Per-game Remix file layout, and the bridge from PCSX2's own settings to the knobs the Remix
// backend reads out of the environment.
//
// WHY THIS EXISTS. Remix keys everything -- its user.conf, its mod (replacement asset) tree, its
// captures -- off the identity of the running executable. PCSX2 is one executable for hundreds
// of games, so out of the box every PS2 title shares a single Remix configuration and a single
// mods folder: tag a texture as sky in one game and it is tagged in all of them, and a
// replacement pack for one game is offered to every other. The same problem Dolphin has, and the
// same shape of answer: give each title its own directory.
//
//     <root>/<SERIAL>/rtx.conf      mod layer, per game
//     <root>/<SERIAL>/user.conf     user layer, per game (what the developer menu writes)
//     <root>/<SERIAL>/mods/         replacement assets, per game
//     <root>/<SERIAL>/captures/     USD captures, per game
//     <root>/<SERIAL>/logs/
//
// HOW THE MOD TREE IS REDIRECTED. dxvk-remix resolves those three roots in
// util::RtxFileSys (src/util/util_filesys.h), and each one has an environment variable override
// baked into its PathSpec table: DEFAULT_MODS_DIR, DXVK_CAPTURE_PATH, DXVK_LOG_PATH. RtxFileSys
// is initialised once while the runtime DLL starts up, so those variables have to be set BEFORE
// remixapi_lib_loadRemixDllAndInitialize. That is the whole reason this runs from
// runtime::initialize and not from the per-frame config refresh.
//
// The serial is available by then: VMManager::UpdateDiscDetails(true) (VMManager.cpp:1467) runs
// during boot, well before "Opening GS...". The one case it does not cover is a title change
// while GS stays open, which leaves the previous game's mod tree bound -- see game_id_at_init.
namespace remix_ps2
{
	namespace paths
	{
		// "Remix"/"EnablePerGameFiles" (default true). When false everything below collapses to
		// the shared, pre-existing behaviour and no environment redirection happens at all.
		bool per_game_enabled();

		// Root holding the per-game folders. "Remix"/"FolderPath", defaulting to
		// <DataRoot>/Remix. Always absolute.
		std::string remix_root();

		// Sanitized disc serial ("SLUS-20883"), or empty when no disc identity is known yet.
		std::string game_id();

		// <remix_root>/<game_id>. Empty when per-game files are off or the serial is unknown --
		// callers MUST treat empty as "no per-game layer", not as a relative path.
		std::string game_dir();

		// <game_dir>/<leaf>, empty under the same conditions as game_dir().
		std::string game_subdir(const char* leaf);

		// Creates <game_dir> and its mods/captures/logs children. Safe to call repeatedly; false
		// only when the tree genuinely could not be created, in which case the caller should fall
		// back to the shared layout rather than redirect Remix at a path that does not exist.
		bool ensure_game_dirs();

		// The runtime DLL to load: PCSX2_REMIX_DLL if set (the A/B harness depends on that
		// winning), else "Remix"/"RuntimePath", else <AppRoot>/remix/d3d9.dll.
		std::wstring runtime_dll();

		// Points RtxFileSys at this game's folders and pushes the PCSX2 settings that map onto
		// PCSX2_REMIX_* knobs into the environment. MUST be called before the runtime DLL loads.
		//
		// An environment variable that is already set is never overwritten, so a harness run
		// (arm.ps1 -Env) still outranks whatever is in the .ini -- the same precedence rule
		// refresh_game_config already follows for the per-game .conf files.
		void apply_before_runtime_load();

		// The serial apply_before_runtime_load bound the mod tree to, for the mismatch warning.
		const std::string& game_id_at_init();

		// Re-applies the knobs the backend re-reads, so a settings change takes effect while the
		// game runs. Latched knobs are deliberately left alone. Call once per frame; it is a
		// handful of settings lookups.
		void apply_live_knobs();
		bool apply_title_env(std::string_view name, std::string_view value);
		bool clear_title_env();
		bool is_external_env(std::string_view name);
		std::string env_value(std::string_view name);
		const char* env_source(std::string_view name);

		// Counts how many times a knob's environment value has actually changed. A backend knob
		// that wants to be live caches its parsed value alongside this and re-reads only when it
		// moves, which keeps GetEnvironmentVariableW out of the per-draw path.
		u64 knob_generation();

		// Force every live_int/live_float to re-parse on its next get(). The per-game conf applier
		// sets environment variables directly rather than through apply_live_knobs(), so without
		// this the counter never moves and a conf-delivered value is only ever seen by knobs whose
		// FIRST read happens to fall after the conf landed. That is not a theory: PCSX2_REMIX_UIMODE
		// and PCSX2_REMIX_UIRASTER were applied in the same conf, in the same millisecond, and read
		// back 1 and 0 respectively -- ui_mode() is first called from the draw path (after), and
		// ui_raster_mode() from OnVSync at frame 0 (before), so one cached the conf value and the
		// other cached the default forever. Eighth instance of this bug class in this project.
		void bump_knob_generation();
	} // namespace paths
} // namespace remix_ps2
