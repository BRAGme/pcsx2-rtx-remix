// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "ui_RemixSettingsWidget.h"

#include "SettingsWidget.h"

#include <QtCore/QString>
#include <vector>

class QGroupBox;
class SettingsInterface;

class RemixSettingsWidget : public SettingsWidget
{
	Q_OBJECT

public:
	RemixSettingsWidget(SettingsWindow* dialog, QWidget* parent);
	~RemixSettingsWidget();

private:
	struct knob_row
	{
		QWidget* label;
		QWidget* editor;
		QGroupBox* group;
		// Label, settings key and group joined, so one contains() serves the filter box.
		QString haystack;
	};

	// Builds a row per entry in the shared knob table. The table is the single source of the
	// label, the default and the settings key, so the page cannot drift from the backend.
	void buildKnobRows(SettingsInterface* sif);
	void applyFilter(const QString& text);

	// Directory the running game's Remix files live in, or empty when nothing is running or
	// per-game files are turned off.
	QString currentGameFolder() const;
	void updateCurrentGameLabel();

	Ui::RemixSettingsWidget m_ui;

	std::vector<knob_row> m_rows;
	std::vector<QGroupBox*> m_groups;
};
