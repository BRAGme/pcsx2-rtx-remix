// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "ui_RemixSettingsWidget.h"

#include "SettingsWidget.h"

class RemixSettingsWidget : public SettingsWidget
{
	Q_OBJECT

public:
	RemixSettingsWidget(SettingsWindow* dialog, QWidget* parent);
	~RemixSettingsWidget();

private:
	// Directory the running game's Remix files live in, or empty when nothing is running or
	// per-game files are turned off.
	QString currentGameFolder() const;
	void updateCurrentGameLabel();

	Ui::RemixSettingsWidget m_ui;
};
