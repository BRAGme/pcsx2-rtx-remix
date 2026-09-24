// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GameRichPresence.h"

#include "MemoryTypes.h"

#include "common/Console.h"
#include "common/StringUtil.h"

#include "fmt/format.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iterator>
#include <string_view>
#include <utility>

// Each supported title reads a handful of globals out of EE memory and fills
// one of these in. Anything it does not know it leaves empty.
namespace
{
	struct Presence
	{
		std::string mode;    // "Terrorist Hunt"
		std::string map;     // "Alpine Village A"
		std::string session; // "Split screen", "Online", "Single player"

		bool empty() const { return mode.empty() && map.empty() && session.empty(); }
	};

	using ProviderFn = bool (*)(Presence& out);

	struct Provider
	{
		const char* serial;
		ProviderFn read;
	};
} // namespace

// The separator Discord shows between the parts of a line, written out as
// explicit UTF-8 so it does not depend on the source file's encoding.
static constexpr const char* SEPARATOR = " \xE2\x80\xA2 ";

// Discord's IPC drops presence updates that arrive faster than roughly one
// every four seconds, so the poll never pushes harder than this.
static constexpr int MIN_UPDATE_INTERVAL_MS = 5000;

// EE memory is only sampled this often. Level and mode changes are not frame
// accurate events and nobody watching Discord can tell the difference.
static constexpr int SAMPLE_INTERVAL_MS = 1000;

static const Provider* s_provider = nullptr;

// What the caller is showing right now.
static std::string s_details;
static std::string s_state;
static bool s_have_presence = false;

// What the last sample said, which is what we want to be showing. These differ
// from the pair above only while an update is being held back off the wire.
static std::string s_pending_details;
static std::string s_pending_state;
static bool s_pending_have_presence = false;

static std::chrono::steady_clock::time_point s_last_sample{};
static std::chrono::steady_clock::time_point s_last_push{};

// ---------------------------------------------------------------------------
// EE memory access
//
// Reads go straight at the emulated RAM rather than through the vtlb, because
// this runs outside of any recompiled block and must never fault, log, or
// disturb emulation state. An address that is not backed by RAM reads as zero
// and the caller gets nothing rather than garbage.
// ---------------------------------------------------------------------------

static bool eeReadBytes(u32 addr, void* dst, u32 size)
{
	if (!eeMem)
		return false;

	const u32 offset = addr & 0x1FFFFFFFu;
	if (offset >= Ps2MemSize::TotalRam || (Ps2MemSize::TotalRam - offset) < size)
		return false;

	std::memcpy(dst, &eeMem->Main[offset], size);
	return true;
}

static u8 eeReadU8(u32 addr, u8 fallback = 0)
{
	u8 value;
	return eeReadBytes(addr, &value, sizeof(value)) ? value : fallback;
}

static u32 eeReadU32(u32 addr, u32 fallback = 0)
{
	u32 value;
	return eeReadBytes(addr, &value, sizeof(value)) ? value : fallback;
}

/// A NUL-terminated string out of EE memory. Returns empty for anything that is
/// not plain printable ASCII, which is what every name we read here is, and
/// which keeps a stale or wild pointer from putting binary into the presence.
static std::string eeReadString(u32 addr, u32 max_length)
{
	std::string out;
	out.reserve(max_length);

	for (u32 i = 0; i < max_length; i++)
	{
		u8 ch;
		if (!eeReadBytes(addr + i, &ch, sizeof(ch)))
			return {};
		if (ch == 0)
			return out;
		if (ch < 0x20 || ch >= 0x7F)
			return {};
		out.push_back(static_cast<char>(ch));
	}

	// Ran off the end without a terminator: not a string we recognise.
	return {};
}

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

static char UpperAscii(char ch)
{
	return (ch >= 'a' && ch <= 'z') ? static_cast<char>(ch - ('a' - 'A')) : ch;
}

static char LowerAscii(char ch)
{
	return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch + ('a' - 'A')) : ch;
}

/// "MOUNTAIN_HIGHWAY" -> "Mountain Highway". Used for any map token that has no
/// hand-written name, so an unmapped or modded level still reads sensibly.
static std::string PrettifyToken(const std::string_view token)
{
	std::string out;
	out.reserve(token.size());

	bool start_of_word = true;
	for (const char ch : token)
	{
		if (ch == '_' || ch == '-')
		{
			if (!out.empty() && out.back() != ' ')
				out.push_back(' ');
			start_of_word = true;
			continue;
		}

		out.push_back(start_of_word ? UpperAscii(ch) : LowerAscii(ch));
		start_of_word = false;
	}

	while (!out.empty() && out.back() == ' ')
		out.pop_back();

	return out;
}

static bool EndsWith(const std::string_view str, const std::string_view suffix)
{
	return str.size() >= suffix.size() && str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// ---------------------------------------------------------------------------
// Rainbow Six 3 (PS2, SLUS-20883)
//
// The retail overlay (SP.SOZ, loaded at its link address) keeps everything we
// need in three fixed places. All three were found by static analysis of the
// stock overlay, sha1 e9bb12138a1e69d551ac9f6b958114e5b2e830da:
//
//   0x006B95E0  char[]  the package name of the level that was loaded last,
//                       e.g. "ALPINES_AOFF" / "PARADE_B_SS" / "PRISON_MP_S".
//                       Written by the level-load path at 0x002F7C08, and read
//                       back directly (as a char, not through a pointer) at
//                       0x001CF334, which is what fixes the layout.
//   0x0065500C  ptr     the game manager singleton. gp is 0x0065B6F0, so this
//                       is the gp-relative global the overlay calls
//                       *(gp-0x66E4).
//     +0x31     u8      game mode, the enum below. Compared against 11 by the
//                       URL builder and against 1 (story mode) by the HUD.
//     +0x90     struct  { char* data; u32 count; u32 max; } holding the game
//                       type class name, e.g. "R6Game.R6TerroristHuntGame".
//                       Read at 0x002FA2CC, written by the URL builder whose
//                       jump table at 0x005EC160 gives the enum its order.
//   0x006546A0  u32     g_bSplitScreen, set at boot from -ssc / -sst.
// ---------------------------------------------------------------------------

namespace R6Three
{
	static constexpr u32 ADDR_LEVEL_PACKAGE = 0x006B95E0;
	static constexpr u32 ADDR_GAME_MANAGER = 0x0065500C;
	static constexpr u32 OFFSET_GAME_MODE = 0x31;
	static constexpr u32 OFFSET_GAME_TYPE = 0x90;
	static constexpr u32 ADDR_SPLIT_SCREEN = 0x006546A0;

	// Index order comes from the URL builder's jump table at 0x005EC160, which
	// maps each value to the R6Game class it loads. 0 is "not set yet".
	struct ModeInfo
	{
		const char* name;
		const char* session; // empty when the package suffix decides it
	};

	static constexpr ModeInfo MODES[] = {
		{nullptr, nullptr},                 // 0  (unset)
		{"Campaign", "Single player"},      // 1  R6StoryModeGame
		{"Practice", "Single player"},      // 2  R6PracticeModeGame
		{"Campaign Co-op", "Online"},       // 3  R6CoopStoryModeGame
		{"Terrorist Hunt", "Single player"},// 4  R6TerroristHuntGame
		{"Terrorist Hunt Co-op", "Online"}, // 5  R6CoopTerroristHuntGame
		{"Deathmatch", "Online"},           // 6  R6DeathMatch
		{"Team Deathmatch", "Online"},      // 7  R6TeamDeathMatchGame
		{"Sharpshooter", "Online"},         // 8  R6SharpShooterGame
		{"Sharpshooter", "Split screen"},   // 9  R6SharpShooterGameForSplitScreen
		{"Terrorist Hunt", "Split screen"}, // 10 R6TerroristHuntGameForSplitScreen
		{"Practice", "Split screen"},       // 11 R6PracticeModeGameForSplitScreen
		{"No Rules", "Online"},             // 12 R6NoRules
	};

	// The class name is the same information as the mode byte, and the overlay
	// keeps both. Reading it as well means a mode the byte has not caught up
	// with still names itself correctly.
	struct GameTypeInfo
	{
		const char* class_name;
		u8 mode;
	};

	static constexpr GameTypeInfo GAME_TYPES[] = {
		{"R6StoryModeGame", 1},
		{"R6PracticeModeGame", 2},
		{"R6CoopStoryModeGame", 3},
		{"R6TerroristHuntGame", 4},
		{"R6CoopTerroristHuntGame", 5},
		{"R6DeathMatch", 6},
		{"R6TeamDeathMatchGame", 7},
		{"R6SharpShooterGame", 8},
		{"R6SharpShooterGameForSplitScreen", 9},
		{"R6TerroristHuntGameForSplitScreen", 10},
		{"R6PracticeModeGameForSplitScreen", 11},
		{"R6NoRules", 12},
	};

	// The 23 level tokens that ship on the disc. The names on the right are how
	// the game presents them to the player; anything not listed falls back to
	// the token with its underscores turned into spaces, so a modded or cut
	// level still shows up as something readable.
	struct MapName
	{
		const char* token;
		const char* name;
	};

	static constexpr MapName MAP_NAMES[] = {
		{"AIRPORT", "Airport"},
		{"AIRPORT2", "Airport"},
		{"ALCATRAZ", "Alcatraz"},
		{"ALPINES", "Alpine Village"},
		{"CROSSFIRE", "Crossfire"},
		{"GARAGE", "Parking Garage"},
		{"IMPORT_EXPORT", "Import/Export"},
		{"ISLAND", "Island Estate"},
		{"MEATPACKING", "Meat Packing Plant"},
		{"MOUNTAIN_HIGHWAY", "Mountain Highway"},
		{"OFFICE_COMPLEX", "Office Complex"},
		{"OIL_REFINERY", "Oil Refinery"},
		{"OLDCITY", "Old City"},
		{"PARADE", "Parade"},
		{"PENTHOUSE", "Penthouse"},
		{"PRESIDIO", "Presidio"},
		{"PRISON", "Prison"},
		{"SANDSTORM", "Sandstorm"},
		{"SHIPYARD", "Shipyard"},
		{"STREETS", "Streets"},
		{"TRAINING_BASICS", "Training: Basics"},
		{"TRAINING_SHOOTING", "Training: Shooting Range"},
		{"TRAINING_TEAM", "Training: Team"},
		{"TRIESTE", "Trieste"},
	};

	/// What the package name's suffix says about the session. The level-load
	/// path builds the name as map + suffix at 0x002F7B34..0x002F7C08.
	struct SuffixInfo
	{
		const char* suffix;
		const char* session; // nullptr = tells us nothing
	};

	static constexpr SuffixInfo SUFFIXES[] = {
		{"_MULTI", "Online"},
		{"_SS", "Split screen"},
		{"OFF", nullptr}, // offline: single player or split screen, mode decides
		{"_S", "Online"}, // hosting
		{"_C", "Online"}, // joined
		{"_D", "Online"}, // dedicated server
	};

	static u8 ReadGameMode()
	{
		const u32 manager = eeReadU32(ADDR_GAME_MANAGER);
		if (manager == 0)
			return 0;

		// The class name is the more trustworthy of the two, because the byte
		// is only rewritten on the paths that care about it.
		const u32 name_ptr = eeReadU32(manager + OFFSET_GAME_TYPE);
		const u32 name_len = eeReadU32(manager + OFFSET_GAME_TYPE + 4);
		if (name_ptr != 0 && name_len > 0 && name_len <= 64)
		{
			const std::string class_name = eeReadString(name_ptr, 63);
			// Drop the package qualifier: "R6Game.R6TerroristHuntGame".
			const std::string_view bare = (class_name.find('.') != std::string::npos) ?
											  std::string_view(class_name).substr(class_name.find('.') + 1) :
											  std::string_view(class_name);
			for (const GameTypeInfo& gt : GAME_TYPES)
			{
				if (bare == gt.class_name)
					return gt.mode;
			}
		}

		const u8 mode = eeReadU8(manager + OFFSET_GAME_MODE);
		return (mode < std::size(MODES)) ? mode : 0;
	}

	static bool Read(Presence& out)
	{
		std::string package = eeReadString(ADDR_LEVEL_PACKAGE, 63);
		if (package.empty())
			return false;

		package = StringUtil::toUpper(package);

		// Strip the session suffix back off, which leaves the map token plus,
		// for the campaign, its part letter.
		const char* suffix_session = nullptr;
		for (const SuffixInfo& si : SUFFIXES)
		{
			if (EndsWith(package, si.suffix))
			{
				package.resize(package.size() - std::strlen(si.suffix));
				suffix_session = si.session;
				break;
			}
		}

		// The menu and the shared package are levels as far as the engine is
		// concerned, but not somewhere the player is playing.
		if (package.empty() || package == "MENU" || package == "COMMON" || package == "ENTRY")
		{
			out.map = "In the menus";
			return true;
		}

		// Campaign levels are split into an A and a B half; multiplayer levels
		// carry _MP instead. Both live on the end of the token.
		std::string part;
		bool multiplayer_map = false;
		if (EndsWith(package, "_MP"))
		{
			package.resize(package.size() - 3);
			multiplayer_map = true;
		}
		else if (package.size() > 2 && package[package.size() - 2] == '_' &&
				 package.back() >= 'A' && package.back() <= 'Z')
		{
			part = package.back();
			package.resize(package.size() - 2);
		}

		std::string map_name;
		for (const MapName& mn : MAP_NAMES)
		{
			if (package == mn.token)
			{
				map_name = mn.name;
				break;
			}
		}
		if (map_name.empty())
			map_name = PrettifyToken(package);

		if (!part.empty())
			map_name += ' ' + part;

		out.map = std::move(map_name);

		const u8 mode = ReadGameMode();
		if (mode != 0 && mode < std::size(MODES) && MODES[mode].name)
		{
			out.mode = MODES[mode].name;
			out.session = MODES[mode].session;
		}

		// The package's own suffix beats the mode table, because it is written
		// by the load that is actually on screen.
		if (suffix_session)
			out.session = suffix_session;
		else if (out.session.empty() && multiplayer_map)
			out.session = "Online";

		// g_bSplitScreen is set from the command line the overlay was relaunched
		// with, so it is the last word on whether there are two players.
		if (eeReadU32(ADDR_SPLIT_SCREEN) != 0)
			out.session = "Split screen";

		return true;
	}
} // namespace R6Three

// ---------------------------------------------------------------------------

static constexpr Provider PROVIDERS[] = {
	{"SLUS-20883", &R6Three::Read}, // Tom Clancy's Rainbow Six 3 (USA)
};

static std::string BuildLine(const std::string& a, const std::string& b)
{
	if (a.empty())
		return b;
	if (b.empty())
		return a;
	return a + SEPARATOR + b;
}

void GameRichPresence::GameChanged(const std::string& serial, u32 crc)
{
	// The CRC is not used yet. It is here because a provider that has to tell a
	// region or a revision apart will need it, and every caller already has it.
	(void)crc;

	const Provider* provider = nullptr;
	for (const Provider& p : PROVIDERS)
	{
		if (serial == p.serial)
		{
			provider = &p;
			break;
		}
	}

	if (provider != s_provider)
	{
		s_provider = provider;
		if (provider)
			DevCon.WriteLn(fmt::format("(GameRichPresence) Detailed presence enabled for {}.", serial));
	}

	s_details.clear();
	s_state.clear();
	s_have_presence = false;
	s_pending_details.clear();
	s_pending_state.clear();
	s_pending_have_presence = false;
	s_last_sample = {};
	s_last_push = {};
}

bool GameRichPresence::IsSupported()
{
	return s_provider != nullptr;
}

bool GameRichPresence::HasPresence()
{
	return s_have_presence;
}

const std::string& GameRichPresence::GetDetails()
{
	return s_details;
}

const std::string& GameRichPresence::GetState()
{
	return s_state;
}

static bool Elapsed(const std::chrono::steady_clock::time_point& since,
	const std::chrono::steady_clock::time_point& now, int interval_ms)
{
	if (since == std::chrono::steady_clock::time_point{})
		return true;

	return std::chrono::duration_cast<std::chrono::milliseconds>(now - since).count() >= interval_ms;
}

bool GameRichPresence::Poll()
{
	if (!s_provider || !eeMem)
		return false;

	const auto now = std::chrono::steady_clock::now();
	if (Elapsed(s_last_sample, now, SAMPLE_INTERVAL_MS))
	{
		s_last_sample = now;

		Presence presence;
		const bool have = s_provider->read(presence) && !presence.empty();

		// The mode joins the game's name on line one; the map and who is
		// playing share line two.
		std::string details = have ? presence.mode : std::string();
		std::string state = have ? BuildLine(presence.map, presence.session) : std::string();

		// With no mode to show, line one would be the title on its own and line
		// two would carry everything. Lift the map up instead.
		if (details.empty() && !presence.map.empty())
		{
			details = presence.map;
			state = have ? presence.session : std::string();
		}

		s_pending_details = std::move(details);
		s_pending_state = std::move(state);
		s_pending_have_presence = have;
	}

	if (s_pending_details == s_details && s_pending_state == s_state &&
		s_pending_have_presence == s_have_presence)
	{
		return false;
	}

	// Hold anything that changes faster than Discord will accept it. The
	// pending text stays put, so a later poll pushes it instead; nothing is
	// lost, it just lands a few seconds late.
	if (!Elapsed(s_last_push, now, MIN_UPDATE_INTERVAL_MS))
		return false;

	s_details = s_pending_details;
	s_state = s_pending_state;
	s_have_presence = s_pending_have_presence;
	s_last_push = now;

	// One line per mode or map change, which is rare enough to be worth having
	// when someone reports that their presence says the wrong thing.
	Console.WriteLn(Color_StrongGreen,
		fmt::format("(GameRichPresence) {} / {}", s_details.empty() ? "-" : s_details.c_str(),
			s_state.empty() ? "-" : s_state.c_str()));

	return true;
}
