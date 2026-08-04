// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "RemixSettingsWidget.h"
#include "QtUtils.h"
#include "SettingWidgetBinder.h"
#include "SettingsWindow.h"

#include "GS/Remix/RemixKnobs.h"
#include "VMManager.h"

#include "common/FileSystem.h"
#include "common/Path.h"

#include <QtCore/QDir>
#include <QtCore/QUrl>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QVBoxLayout>

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

	buildKnobRows(sif);

	connect(m_ui.filter, &QLineEdit::textChanged, this, &RemixSettingsWidget::applyFilter);

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
}

RemixSettingsWidget::~RemixSettingsWidget() = default;

void RemixSettingsWidget::buildKnobRows(SettingsInterface* sif)
{
	size_t count = 0;
	const remix_ps2::knob* table = remix_ps2::knobs(count);

	// A game is running, so anything read once at backend start is already captured and editing it
	// now would silently do nothing until a restart. Saying so beats letting the user change a
	// value and conclude the feature is broken.
	const bool running = VMManager::HasValidVM();

	QGroupBox* group = nullptr;
	QFormLayout* form = nullptr;
	QString current_group;

	for (size_t i = 0; i < count; ++i)
	{
		const remix_ps2::knob& k = table[i];
		const QString group_name = QString::fromUtf8(k.group);

		if (group_name != current_group)
		{
			current_group = group_name;
			group = new QGroupBox(group_name, m_ui.knobContainer);
			form = new QFormLayout(group);
			m_ui.knobLayout->addWidget(group);
			m_groups.push_back(group);
		}

		QWidget* editor = nullptr;
		const QString key = QString::fromUtf8(k.env);

		switch (k.type)
		{
			case remix_ps2::knob_type::Boolean:
			{
				QCheckBox* box = new QCheckBox(group);
				SettingWidgetBinder::BindWidgetToBoolSetting(
					sif, box, "Remix", k.env, k.default_value != 0.0);
				editor = box;
				break;
			}

			case remix_ps2::knob_type::Choice:
			{
				QComboBox* box = new QComboBox(group);
				for (const QString& entry : QString::fromUtf8(k.choices).split('|'))
					box->addItem(entry);

				SettingWidgetBinder::BindWidgetToIntSetting(
					sif, box, "Remix", k.env, static_cast<int>(k.default_value));
				editor = box;
				break;
			}

			case remix_ps2::knob_type::Integer:
			{
				QSpinBox* box = new QSpinBox(group);
				box->setRange(static_cast<int>(k.minimum), static_cast<int>(k.maximum));
				box->setSingleStep(std::max(1, static_cast<int>(k.step)));
				SettingWidgetBinder::BindWidgetToIntSetting(
					sif, box, "Remix", k.env, static_cast<int>(k.default_value));
				editor = box;
				break;
			}

			case remix_ps2::knob_type::Float:
			{
				QDoubleSpinBox* box = new QDoubleSpinBox(group);
				box->setRange(k.minimum, k.maximum);
				box->setSingleStep(k.step);
				// Enough places for the small gates (WFLAT defaults to 0.001) without turning the
				// large ones into a wall of zeroes.
				box->setDecimals((k.step < 0.01) ? 4 : 2);
				SettingWidgetBinder::BindWidgetToFloatSetting(
					sif, box, "Remix", k.env, static_cast<float>(k.default_value));
				editor = box;
				break;
			}
		}

		if (!editor)
			continue;

		QString label_text = QString::fromUtf8(k.label);
		if (k.latched)
		{
			label_text += tr(" (restart)");
			editor->setEnabled(!running);
		}

		QLabel* label = new QLabel(label_text, group);
		form->addRow(label, editor);

		// The key is in the tooltip so a value seen in the .ini or a per-game .conf can be traced
		// back to the control that sets it, and so the filter can match on it.
		const QString tip = QString::fromUtf8(k.tooltip) +
			tr("\n\nSetting: [Remix] %1   Environment: PCSX2_REMIX_%1").arg(key) +
			(k.latched ? tr("\nRead once when the backend starts; applies after a restart.") : QString());

		label->setToolTip(tip);
		editor->setToolTip(tip);

		m_rows.push_back({label, editor, group, label_text + QLatin1Char(' ') + key + QLatin1Char(' ') + group_name});
	}
}

void RemixSettingsWidget::applyFilter(const QString& text)
{
	const QString needle = text.trimmed();

	for (const knob_row& row : m_rows)
	{
		const bool match = needle.isEmpty() || row.haystack.contains(needle, Qt::CaseInsensitive);
		row.label->setVisible(match);
		row.editor->setVisible(match);
	}

	// A group whose every row was filtered out is just an empty box taking up space.
	for (QGroupBox* group : m_groups)
	{
		bool any = false;
		for (const knob_row& row : m_rows)
		{
			if (row.group == group && row.editor->isVisible())
			{
				any = true;
				break;
			}
		}

		group->setVisible(any);
	}
}

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
