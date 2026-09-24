// SPDX-FileCopyrightText: 2002-2025 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <string>

/// Game-aware Discord Rich Presence.
///
/// Discord already gets the game's name from the game list. For the handful of
/// titles mapped here it also gets what the player is actually doing: the game
/// mode, the map, and whether it is single player, split screen or online. That
/// comes from reading a few known globals out of EE memory, so a title is only
/// supported once someone has found its addresses.
namespace GameRichPresence
{
	/// Point the presence at a different game. Pass an empty serial on shutdown.
	void GameChanged(const std::string& serial, u32 crc);

	/// True when the running game has a provider, whether or not it has
	/// anything to say yet.
	bool IsSupported();

	/// Samples EE memory and rebuilds the presence text. Returns true when the
	/// text changed and the presence is worth pushing to Discord again.
	/// CPU thread only; cheap enough to call once a frame, but it rate-limits
	/// itself internally, so callers do not need to.
	bool Poll();

	/// True when the provider currently knows something. Both strings are empty
	/// otherwise, and the caller should fall back to whatever it used before.
	bool HasPresence();

	/// What to add to line one, after the game's name, e.g. "Terrorist Hunt".
	const std::string& GetDetails();

	/// Line two on its own, e.g. "Alpine Village A - Split screen".
	const std::string& GetState();
} // namespace GameRichPresence
