// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <cstddef>

// The Remix backend's tunable knobs, declared once and shared by the two things that need them:
// the settings page that draws them, and the bridge that pushes them into the environment the
// backend reads.
//
// WHY A TABLE. There are ~80 PCSX2_REMIX_* knobs. Hand-writing a widget per knob in a .ui file
// means every new knob needs edits in three places and drifts out of sync with the code that
// reads it. Declaring them once keeps the label, the default shown in the GUI and the default the
// backend actually applies from ever disagreeing.
//
// `env` is the whole identity: the environment variable is PCSX2_REMIX_<env>, and the settings
// key is [Remix]/<env>. One string, so a knob cannot be wired to the wrong variable.
//
// `latched` marks knobs the backend reads through a `static const` local, so the value is captured
// the first time that code runs and never re-read. Those only take effect on a restart, and the
// settings page greys them out while a game is running to say so. The rest are re-read, and the
// bridge re-applies them about once a second so they respond while the game runs.
namespace remix_ps2
{
	enum class knob_type
	{
		Boolean, // rendered as a checkbox; stored 0/1
		Integer,
		Float,
		Choice, // Integer, but rendered as a combo box over `choices`
	};

	struct knob
	{
		const char* env; // PCSX2_REMIX_<env>, and the [Remix]/<env> settings key
		const char* group;
		const char* label;
		knob_type type;
		double default_value;
		double minimum;
		double maximum;
		double step;
		// Pipe-separated combo entries, in value order starting at 0. Null unless type is Choice.
		const char* choices;
		const char* tooltip;
		bool latched; // read once at backend start; needs a restart
	};

	// The table. `count` receives the number of entries.
	const knob* knobs(size_t& count);
} // namespace remix_ps2
