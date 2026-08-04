// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "RemixSettingsWidget.h"
#include "QtUtils.h"
#include "SettingWidgetBinder.h"
#include "SettingsWindow.h"

#include "VMManager.h"

#include "common/FileSystem.h"
#include "common/Path.h"

#include <QtCore/QDir>
#include <QtCore/QUrl>
#include <QtWidgets/QFileDialog>

RemixSettingsWidget::RemixSettingsWidget(SettingsWindow* settings_dialog, QWidget* parent)
	: SettingsWidget(settings_dialog, parent)
{
	SettingsInterface* sif = dialog()->getSettingsInterface();

	setupTab(m_ui);

	// The runtime DLL is a file, not a folder, so it cannot use BindWidgetToFolderSetting.
	SettingWidgetBinder::BindWidgetToStringSetting(sif, m_ui.runtimePath, "Remix", "RuntimePath", std::string());

	connect(m_ui.runtimeBrowse, &QPushButton::clicked, this, [this]() {
		const QString path = QFileDialog::getOpenFileName(QtUtils::GetRootWidget(this),
			tr("Select RTX Remix Runtime"), m_ui.runtimePath->text(),
			tr("Remix Runtime (d3d9*.dll);;All Files (*.*)"));
		if (!path.isEmpty())
			m_ui.runtimePath->setText(QDir::toNativeSeparators(path));
	});

	connect(m_ui.runtimeReset, &QPushButton::clicked, this, [this]() { m_ui.runtimePath->clear(); });

	SettingWidgetBinder::BindWidgetToBoolSetting(sif, m_ui.perGameFiles, "Remix", "EnablePerGameFiles", true);
	SettingWidgetBinder::BindWidgetToFolderSetting(sif, m_ui.remixFolder, m_ui.remixFolderBrowse,
		m_ui.remixFolderOpen, m_ui.remixFolderReset, "Remix", "FolderPath",
		Path::Combine(EmuFolders::DataRoot, "RemixGames"));

	SettingWidgetBinder::BindWidgetToFloatSetting(sif, m_ui.worldScale, "Remix", "WorldScale", 8.0f);
	SettingWidgetBinder::BindWidgetToIntSetting(sif, m_ui.lightMode, "Remix", "LightMode", 1);
	SettingWidgetBinder::BindWidgetToFloatSetting(sif, m_ui.lightBrightness, "Remix", "LightBrightness", 100.0f);
	SettingWidgetBinder::BindWidgetToFloatSetting(sif, m_ui.ambientBrightness, "Remix", "AmbientBrightness", 0.0f);
	SettingWidgetBinder::BindWidgetToFloatSetting(sif, m_ui.lightAngle, "Remix", "LightAngle", 8.0f);

	connect(m_ui.perGameFiles, &QCheckBox::checkStateChanged, this, [this]() { updateCurrentGameLabel(); });
	connect(m_ui.remixFolder, &QLineEdit::textChanged, this, [this]() { updateCurrentGameLabel(); });

	connect(m_ui.openGameFolder, &QPushButton::clicked, this, [this]() {
		const QString folder = currentGameFolder();
		if (folder.isEmpty())
			return;

		// The folder is only created by the GS thread at boot, so opening it before the game has
		// ever run would otherwise fail. Creating it here also gives somewhere to drop a
		// replacement pack ahead of the first launch.
		const std::string narrow(folder.toStdString());
		FileSystem::EnsureDirectoryExists(Path::Combine(narrow, "mods").c_str(), true);

		QtUtils::OpenURL(this, QUrl::fromLocalFile(folder));
	});

	updateCurrentGameLabel();

	dialog()->registerWidgetHelp(m_ui.perGameFiles, tr("Separate Remix Files Per Game"), tr("Checked"),
		tr("Gives each disc serial its own Remix folder holding rtx.conf, user.conf and a mods "
		   "directory. Without this every PS2 game shares one Remix configuration and one set of "
		   "replacement assets, so texture tags and mods leak between titles."));
	dialog()->registerWidgetHelp(m_ui.remixFolder, tr("Remix Folder"), tr("RemixGames"),
		tr("Parent folder holding the per-game Remix directories. It deliberately does not default "
		   "to \"Remix\", which on Windows would be the same directory as the runtime's own "
		   "remix\\ folder."));
	dialog()->registerWidgetHelp(m_ui.runtimePath, tr("Runtime DLL"), tr("Empty"),
		tr("RTX Remix runtime to load. Leave empty to use remix\\d3d9.dll beside PCSX2."));
	dialog()->registerWidgetHelp(m_ui.worldScale, tr("World Scale"), tr("8.00"),
		tr("Scale applied when converting PS2 units into Remix world units. Affects how large the "
		   "scene appears to lights and to the path tracer."));
	dialog()->registerWidgetHelp(m_ui.lightMode, tr("Lighting Mode"), tr("Dome + key light"),
		tr("How the scene is lit when the game's own lights are not reconstructed. A dome and a "
		   "distant light have no distance falloff, so one brightness works whether the scene is "
		   "a corridor or an outdoor map; a point light at the camera does not."));
	dialog()->registerWidgetHelp(m_ui.lightBrightness, tr("Key Light Brightness"), tr("100.00"),
		tr("Radiance of the key light."));
	dialog()->registerWidgetHelp(m_ui.ambientBrightness, tr("Ambient Brightness"), tr("0.00"),
		tr("Radiance of the ambient dome. Raise to lift shadows that read as pure black."));
	dialog()->registerWidgetHelp(m_ui.lightAngle, tr("Key Light Angular Size"), tr("8.00 degrees"),
		tr("Angular diameter of the key light. Smaller is sharper shadows, larger is softer."));
}

RemixSettingsWidget::~RemixSettingsWidget() = default;

QString RemixSettingsWidget::currentGameFolder() const
{
	if (!m_ui.perGameFiles->isChecked())
		return QString();

	// Per-game settings windows carry their own serial; the global one has to ask the running VM.
	std::string serial = dialog()->getSerial();
	if (serial.empty())
		serial = VMManager::GetDiscSerial();

	if (serial.empty())
		return QString();

	std::string root(m_ui.remixFolder->text().toStdString());
	if (root.empty())
		root = Path::Combine(EmuFolders::DataRoot, "RemixGames");
	else if (!Path::IsAbsolute(root))
		root = Path::Combine(EmuFolders::DataRoot, root);

	return QString::fromStdString(Path::Combine(root, Path::SanitizeFileName(serial)));
}

void RemixSettingsWidget::updateCurrentGameLabel()
{
	const QString folder = currentGameFolder();

	if (folder.isEmpty())
	{
		m_ui.currentGameLabel->setText(
			m_ui.perGameFiles->isChecked() ?
				tr("No game running -- start a game to see its Remix folder.") :
				tr("Per-game files are off; every game shares one Remix configuration."));
	}
	else
	{
		m_ui.currentGameLabel->setText(tr("This game: %1").arg(QDir::toNativeSeparators(folder)));
	}

	m_ui.openGameFolder->setEnabled(!folder.isEmpty());
}

#include "moc_RemixSettingsWidget.cpp"
