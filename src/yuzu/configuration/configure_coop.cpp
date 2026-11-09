// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "yuzu/configuration/configure_coop.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <fmt/format.h>
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/settings.h"
#include "qt_common/config/qt_config.h"
#include "yuzu/coop/coop_boot.h"

namespace {
namespace Gui = Core::OotCoop::Gui;
QString Q(const std::string& value) { return QString::fromStdString(value); }
void EnvTip(QWidget* field, const char* name) {
    if (CoopEnvironmentIsExternal(name))
        field->setToolTip(QObject::tr("overridden by %1 from the environment").arg(QString::fromLatin1(name)));
}
} // namespace

ConfigureCoop::ConfigureCoop(u64 title_id_, QtConfig* config_, QWidget* parent)
    : QWidget(parent), config(config_), title_id(title_id_) {
    auto* outer = new QVBoxLayout(this);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    outer->addWidget(scroll);
    auto* page = new QWidget(scroll);
    scroll->setWidget(page);
    auto* layout = new QVBoxLayout(page);
    enabled = new QCheckBox(tr("Enable co-op for this game"), page);
    layout->addWidget(enabled);
    EnvTip(enabled, "EDEN_NX2_OOT_COOP");
    auto* connection = new QGroupBox(tr("Connection"), page);
    auto* form = new QFormLayout(connection);
    const auto line = [&](const QString& label, const char* env) {
        auto* edit = new QLineEdit(connection);
        form->addRow(label, edit);
        EnvTip(edit, env);
        return edit;
    };
    host = line(tr("Relay host"), "EDEN_NX2_OOT_COOP_RELAY");
    port = new QSpinBox(connection);
    port->setRange(1, 65535);
    form->addRow(tr("Port"), port);
    EnvTip(port, "EDEN_NX2_OOT_COOP_RELAY");
    room = line(tr("Room"), "EDEN_NX2_OOT_COOP_ROOM");
    name = line(tr("Player name"), "EDEN_NX2_OOT_COOP_NAME");
    team = line(tr("Team"), "EDEN_NX2_OOT_COOP_TEAM");
    seed = line(tr("Seed hash"), "EDEN_NX2_OOT_COOP_SEED");
    form->addRow(new QLabel(tr("0 = vanilla; the randomizer launcher's seed hash"), connection));
    layout->addWidget(connection);

    auto* kit = new QGroupBox(tr("Kit"), page);
    auto* kit_form = new QFormLayout(kit);
    auto* path_row = new QHBoxLayout;
    keys = new QLineEdit(kit);
    EnvTip(keys, "EDEN_NX2_OOT_COOP_KEYS");
    auto* browse = new QPushButton(tr("Browse..."), kit);
    path_row->addWidget(keys);
    path_row->addWidget(browse);
    kit_form->addRow(tr("Key file"), path_row);
    kit_info = new QLabel(kit);
    kit_info->setTextFormat(Qt::PlainText);
    kit_info->setTextInteractionFlags(Qt::TextSelectableByMouse);
    kit_form->addRow(kit_info);
    policy = new QLineEdit(kit);
    policy->setPlaceholderText(tr("Automatic from key file"));
    EnvTip(policy, "EDEN_NX2_OOT_COOP_POLICY");
    kit_form->addRow(tr("Policy override"), policy);
    policy_info = new QLabel(kit);
    policy_info->setTextFormat(Qt::PlainText);
    kit_form->addRow(policy_info);
    layout->addWidget(kit);
    connect(browse, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getOpenFileName(this, tr("Co-op key file"), keys->text(), tr("JSON files (*.json)"));
        if (!path.isEmpty()) { keys->setText(path); RefreshKit(); }
    });
    connect(keys, &QLineEdit::editingFinished, this, &ConfigureCoop::RefreshKit);

    auto* sync = new QGroupBox(tr("Sync classes"), page);
    auto* sync_layout = new QHBoxLayout(sync);
    constexpr std::array tokens{"bool", "bit", "rupee", "item", "pose", "dungeon", "quest"};
    for (size_t i = 0; i < classes.size(); ++i) {
        classes[i] = new QCheckBox(QString::fromLatin1(tokens[i]), sync);
        EnvTip(classes[i], "EDEN_NX2_OOT_COOP_ALLOW");
        sync_layout->addWidget(classes[i]);
    }
    layout->addWidget(sync);
    auto* switches = new QGroupBox(tr("Session switches"), page);
    auto* switches_form = new QFormLayout(switches);
    const auto combo = [&](const QString& title, const QStringList& entries) {
        auto* box = new QComboBox(switches);
        box->addItems(entries);
        switches_form->addRow(title, box);
        EnvTip(box, "EDEN_NX2_OOT_COOP_KEYS");
        return box;
    };
    quest = combo(tr("Quest sync"), {tr("Key file"), tr("On"), tr("Off")});
    teleport = combo(tr("Teleport"), {tr("Key file"), tr("On"), tr("Off")});
    cleanup = combo(tr("World cleanup"), {tr("Key file"), tr("Death"), tr("Flag only")});
    tint = new QCheckBox(tr("Ghost tint"), switches);
    EnvTip(tint, "EDEN_NX2_OOT_COOP_TINT");
    switches_form->addRow(tint);
    ghost = new QCheckBox(switches);
    switches_form->addRow(ghost);
    connect(ghost, &QCheckBox::clicked, this, [this] { ghost_changed = true; });
    layout->addWidget(switches);
    auto* note = new QLabel(tr("Environment variables set by a launcher override these values. "
        "Changing switches makes a session copy of the key file; both players must use the same "
        "switches (the mod hash covers the key file).\n"
        "After changing co-op enablement, close and reopen the application: the runtime caches it at first boot."), page);
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color: gray"));
    layout->addWidget(note);
    error = new QLabel(page);
    error->setWordWrap(true);
    error->setStyleSheet(QStringLiteral("color: red"));
    layout->addWidget(error);
    layout->addStretch();
    LoadConfiguration();
    if (const auto shots = qEnvironmentVariable("EDEN_NX2_OOT_COOP_GUI_SHOTS"); !shots.isEmpty()) {
        // Field-test hook: bring this tab forward and save grabs of the dialog and the whole page.
        QTimer::singleShot(1500, this, [this, shots, page] {
            for (auto* parent = parentWidget(); parent; parent = parent->parentWidget())
                if (auto* tabs = qobject_cast<QTabWidget*>(parent)) { tabs->setCurrentWidget(this); break; }
            QTimer::singleShot(1000, this, [this, shots, page] {
                const QDir dir(shots);
                window()->grab().save(dir.filePath(QStringLiteral("page-dialog.png")));
                page->grab().save(dir.filePath(QStringLiteral("page-full.png")));
            });
        });
    }
}
void ConfigureCoop::LoadConfiguration() {
    saved = Gui::FromIni(config->ReadRawSection("Coop"));
    enabled->setChecked(saved.enabled);
    const auto colon = saved.relay.rfind(':');
    host->setText(Q(saved.relay.substr(0, colon)));
    port->setValue(std::stoi(saved.relay.substr(colon + 1)));
    room->setText(Q(saved.room)); name->setText(Q(saved.name)); team->setText(Q(saved.team));
    seed->setText(Q(saved.seed)); keys->setText(Q(saved.keys_path)); policy->setText(Q(saved.policy));
    const auto allow = Gui::ParseAllow(saved.allow).value();
    for (auto* box : classes) box->setChecked(allow.contains(box->text().toStdString()));
    quest->setCurrentIndex(static_cast<int>(saved.quest));
    teleport->setCurrentIndex(static_cast<int>(saved.teleport));
    cleanup->setCurrentIndex(static_cast<int>(saved.cleanup));
    tint->setChecked(saved.tint);
    const auto& disabled = Settings::values.disabled_addons[title_id];
    ghost->setChecked(std::find(disabled.begin(), disabled.end(), saved.ghost_mod) == disabled.end());
    const auto folder = Common::FS::GetEdenPath(Common::FS::EdenPath::LoadDir) /
                        fmt::format("{:016X}", title_id) / std::filesystem::path(Common::FS::ToU8String(saved.ghost_mod));
    std::error_code ec;
    const bool installed = std::filesystem::is_directory(folder, ec);
    ghost->setText(tr("Ghost mod (%1)%2").arg(Q(saved.ghost_mod), installed ? QString{} : tr(" — not installed")));
    ghost->setEnabled(installed);
    ghost_changed = false;
    RefreshKit();
}
void ConfigureCoop::RefreshKit() {
    const auto path = std::filesystem::path(Common::FS::ToU8String(keys->text().toStdString()));
    QStringList lines;
    for (const auto& file : Gui::DescribeKit(path))
        lines << tr("%1  %2 bytes  sha256:%3").arg(Q(file.name)).arg(file.size).arg(Q(file.sha256_12.value_or("unreadable")));
    kit_info->setText(lines.isEmpty() ? tr("No kit files found") : lines.join(QLatin1Char{'\n'}));
    nlohmann::ordered_json file;
    try {
        std::ifstream in(path, std::ios::binary);
        if (in) in >> file;
    } catch (const nlohmann::json::exception&) {
        file = nullptr;
    }
    const auto digest = Gui::PolicyDigest(file);
    policy_info->setText(digest.empty() ? tr("Policy: unavailable (select a valid key file)") :
        tr("Policy: %1 (auto)").arg(Q(digest)));
    const auto view = Gui::InspectSwitches(file);
    const auto show = [&](QComboBox* box, const Gui::Switch& value) {
        box->setEnabled(value.applicable);
        box->setItemText(0, value.applicable ? tr("Key file (key file: %1)").arg(Q(value.value)) :
            tr("Key file (not in this key file)"));
        if (!value.applicable) box->setCurrentIndex(0);
    };
    show(quest, view.quest); show(teleport, view.teleport); show(cleanup, view.cleanup);
}
void ConfigureCoop::ApplyConfiguration() {
    auto next = saved;
    QStringList errors;
    const auto endpoint = host->text().toStdString() + ":" + std::to_string(port->value());
    if (Gui::ValidEndpoint(endpoint)) next.relay = endpoint;
    else errors << tr("Relay endpoint was not saved: use a host and port without spaces.");
    const auto token = [&](QLineEdit* edit, std::string& value, const QString& label) {
        const auto text = edit->text().toStdString();
        if (Gui::ValidToken(text)) value = text;
        else errors << tr("%1 was not saved: use 1–64 printable ASCII characters.").arg(label);
    };
    token(room, next.room, tr("Room")); token(team, next.team, tr("Team"));
    token(name, next.name, tr("Player name")); token(seed, next.seed, tr("Seed hash"));
    next.enabled = enabled->isChecked(); next.tint = tint->isChecked();
    next.keys_path = keys->text().toStdString(); next.policy = policy->text().toStdString();
    next.quest = static_cast<Gui::Override>(quest->currentIndex());
    next.teleport = static_cast<Gui::Override>(teleport->currentIndex());
    next.cleanup = static_cast<Gui::Cleanup>(cleanup->currentIndex());
    std::set<std::string> allow;
    for (const auto* box : classes) if (box->isChecked()) allow.insert(box->text().toStdString());
    next.allow = Gui::FormatAllow(allow);
    if (ghost_changed) {
        auto& disabled = Settings::values.disabled_addons[title_id];
        std::erase(disabled, next.ghost_mod);
        if (!ghost->isChecked()) disabled.push_back(next.ghost_mod);
        std::sort(disabled.begin(), disabled.end());
        // Same invalidation as the Add-Ons tab: the game list caches the add-on column.
        Common::FS::RemoveFile(Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir) / "game_list" /
                               fmt::format("{:016X}.pv.txt", title_id));
    }
    config->WriteRawSection("Coop", Gui::ToIni(next));
    saved = std::move(next);
    error->setText(errors.join(QLatin1Char{'\n'}));
}
