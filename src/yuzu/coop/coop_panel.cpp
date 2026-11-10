// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "yuzu/coop/coop_panel.h"

#include <algorithm>
#include <chrono>
#include <QAction>
#include <QCheckBox>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QFormLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <fmt/format.h>
#include "common/fs/path_util.h"
#include "common/settings.h"
#include "core/arm/dynarmic/oot_coop_gui.h"
#include "core/arm/dynarmic/oot_coop_ledger.h"
#include "core/core.h"

namespace {
namespace Gui = Core::OotCoop::Gui;
QString Q(const std::string& text) { return QString::fromStdString(text); }
std::string Env(std::string_view name) { return qgetenv(std::string{name}.c_str()).toStdString(); }
u64 Now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
QString Seen(u64 now, u64 seen) {
    if (!seen) return QStringLiteral("-");
    const auto seconds = now > seen ? (now - seen) / 1000 : 0;
    if (seconds < 2) return QObject::tr("now");
    return seconds < 60 ? QObject::tr("%1 s").arg(seconds) : QObject::tr("%1 min").arg(seconds / 60);
}
} // namespace

CoopPanel::CoopPanel(Core::System& system_, QWidget* parent)
    : QWidget(parent, Qt::Tool), system(system_) {
    setWindowTitle(tr("Co-op Panel"));
    resize(760, 530);
    auto* layout = new QVBoxLayout(this);
    const auto label = [&] {
        auto* item = new QLabel(this);
        item->setTextFormat(Qt::PlainText);
        item->setWordWrap(true);
        layout->addWidget(item);
        return item;
    };
    header = label(); identity = label(); local = label();
    peers = new QTableWidget(0, 6, this);
    peers->setHorizontalHeaderLabels({tr("Name"), tr("Online"), tr("Age"), tr("Area"), tr("Last seen"), tr("Teleport to")});
    peers->setEditTriggers(QAbstractItemView::NoEditTriggers);
    peers->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    peers->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(peers);
    auto* buttons = new QHBoxLayout;
    auto* resync = resync_button = new QPushButton(tr("Resync (request team state)"), this);
    buttons->addWidget(resync);
    room_window = new QCheckBox(tr("Show room window"), this);
    buttons->addWidget(room_window);
    layout->addLayout(buttons);
    message = label();
    connect(resync, &QPushButton::clicked, this, [this] {
        message->setText(Gui::RequestResync(Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir)) ?
            tr("Team state requested") : tr("Could not write resync request"));
    });
    connect(room_window, &QCheckBox::clicked, this, [this](bool shown) {
        message->setText(Gui::SetRoomWindow(Gui::RoomTrigger(Env), shown) ?
            tr("Room window preference saved") : tr("Could not update room window trigger"));
        Refresh();
    });
    auto* connection = new QHBoxLayout;
    auto* reconnect = reconnect_button = new QPushButton(tr("Reconnect with page settings"), this);
    reconnect->setToolTip(tr("Restarts the game after confirmation. Launcher values still win."));
    connect(reconnect, &QPushButton::clicked, this, [this] {
        message->setText(tr("Restart with page settings requested"));
        emit ReconnectRequested();
    });
    connection->addWidget(reconnect);
    disconnect_button = new QPushButton(tr("Disconnect"), this);
    connect_button = new QPushButton(tr("Connect"), this);
    connection->addWidget(disconnect_button);
    connection->addWidget(connect_button);
    connect(disconnect_button, &QPushButton::clicked, this, [this] {
        message->setText(Gui::RequestDisconnect(Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir)) ?
            tr("Relay disconnect requested") : tr("Could not write disconnect request"));
        Refresh();
    });
    connect(connect_button, &QPushButton::clicked, this, [this] {
        message->setText(Gui::RequestConnect(Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir)) ?
            tr("Relay connect requested") : tr("Could not remove disconnect request"));
        Refresh();
    });
    layout->addLayout(connection);
    auto* group = new QGroupBox(tr("Randomizer"), this);
    auto* form = new QFormLayout(group);
    const QStringList titles{tr("Seed hash"), tr("Mod hash"), tr("Policy"), tr("Mod folders in use")};
    for (size_t i = 0; i < randomizer.size(); ++i) {
        auto* row = new QHBoxLayout;
        auto* value = new QLabel(group);
        value->setTextFormat(Qt::PlainText);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        value->setWordWrap(true);
        randomizer[i] = value;
        row->addWidget(value, 1);
        auto* copy = new QPushButton(tr("Copy"), group);
        row->addWidget(copy);
        connect(copy, &QPushButton::clicked, this, [this, i] {
            QGuiApplication::clipboard()->setText(copy_values[i]);
        });
        form->addRow(titles[static_cast<int>(i)], row);
    }
    auto* copy_all = copy_all_button = new QPushButton(tr("Copy all"), group);
    form->addRow(copy_all);
    connect(copy_all, &QPushButton::clicked, this, [this] {
        QGuiApplication::clipboard()->setText(QStringLiteral("seed=%1 mod_hash=%2 policy=%3 mods=%4")
            .arg(copy_values[0], copy_values[1], copy_values[2], copy_values[3]));
    });
    layout->addWidget(group);
    action = new QAction(tr("Co-op Panel"), this);
    action->setCheckable(true);
    connect(action, &QAction::toggled, this, &QWidget::setVisible);
    timer = new QTimer(this);
    timer->setInterval(500);
    connect(timer, &QTimer::timeout, this, &CoopPanel::Refresh);
    shots = qEnvironmentVariable("EDEN_NX2_OOT_COOP_GUI_SHOTS");
    if (!shots.isEmpty()) {
        // Field tests run headless: show the panel without a click and service the command file.
        QTimer::singleShot(1000, this, [this] { show(); });
        auto* hook = new QTimer(this);
        hook->setInterval(1000);
        connect(hook, &QTimer::timeout, this, &CoopPanel::TestHook);
        hook->start();
    }
}
void CoopPanel::TestHook() {
    const QDir dir(shots);
    const auto save = [&](const QString& name) {
        const bool ok = grab().save(dir.filePath(name));
        QFile log(dir.filePath(QStringLiteral("panel.log")));
        if (log.open(QIODevice::Append | QIODevice::Text))
            log.write(QStringLiteral("%1 shot %2 %3\n").arg(Now()).arg(name).arg(ok ? QStringLiteral("ok") : QStringLiteral("failed")).toUtf8());
    };
    QFile commands(dir.filePath(QStringLiteral("panel.cmd")));
    if (commands.exists() && commands.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const auto lines = QString::fromUtf8(commands.readAll()).split(QLatin1Char{'\n'}, Qt::SkipEmptyParts);
        commands.close();
        commands.remove();
        for (const auto& line : lines) {
            const auto words = line.trimmed().split(QLatin1Char{' '}, Qt::SkipEmptyParts);
            if (words.isEmpty()) continue;
            QString result = QStringLiteral("unknown command");
            if (words[0] == QStringLiteral("teleport") && words.size() == 2) {
                auto* button = qobject_cast<QPushButton*>(peers->cellWidget(words[1].toInt(), 5));
                result = !button ? QStringLiteral("no such row") : !button->isEnabled() ? QStringLiteral("button disabled")
                                 : (button->click(), message->text());
            } else if (words[0] == QStringLiteral("resync")) {
                resync_button->click(); result = message->text();
            } else if (words[0] == QStringLiteral("disconnect") || words[0] == QStringLiteral("connect") ||
                       words[0] == QStringLiteral("reconnect")) {
                Refresh();
                auto* button = words[0] == QStringLiteral("disconnect") ? disconnect_button :
                    words[0] == QStringLiteral("connect") ? connect_button : reconnect_button;
                result = button->isEnabled() ? (button->click(), message->text()) : QStringLiteral("button disabled");
            } else if (words[0] == QStringLiteral("room") && words.size() == 2) {
                if (room_window->isChecked() != (words[1] == QStringLiteral("on"))) room_window->click();
                result = message->text() + QStringLiteral(" checked=%1").arg(room_window->isChecked());
            } else if (words[0] == QStringLiteral("copyall")) {
                copy_all_button->click();
                result = QStringLiteral("clipboard: ") + QGuiApplication::clipboard()->text();
            } else if (words[0] == QStringLiteral("shot") && words.size() == 2) {
                Refresh(); save(QStringLiteral("panel-%1.png").arg(words[1])); result = QStringLiteral("saved");
            }
            QFile log(dir.filePath(QStringLiteral("panel.log")));
            if (log.open(QIODevice::Append | QIODevice::Text))
                log.write(QStringLiteral("%1 %2 -> %3\n").arg(Now()).arg(line.trimmed(), result).toUtf8());
        }
    }
    const auto serial = Gui::CurrentStatus().serial;
    if (isVisible() && (++shot_count % 5 == 0 || serial != shot_serial)) {
        shot_serial = serial;
        save(QStringLiteral("panel-latest.png"));
    }
}
QAction* CoopPanel::toggleViewAction() { return action; }
void CoopPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    const QSignalBlocker block(action);
    action->setChecked(true);
    Refresh();
    timer->start();
}
void CoopPanel::hideEvent(QHideEvent* event) {
    timer->stop();
    const QSignalBlocker block(action);
    action->setChecked(false);
    QWidget::hideEvent(event);
}
void CoopPanel::Refresh() {
    const auto status = Gui::CurrentStatus();
    const auto now = Now();
    const bool live = status.live && status.published_ms && now >= status.published_ms &&
                      now - status.published_ms <= 3000;
    auto relay = Env("EDEN_NX2_OOT_COOP_RELAY");
    if (relay.empty()) {
        const auto enable = Env("EDEN_NX2_OOT_COOP");
        if (enable != "1") relay = enable;
    }
    header->setText(live ? tr("Relay %1 — %2 — ping %3 ms").arg(Q(relay), Q(status.relay_state),
        status.rtt_ms ? QString::number(*status.rtt_ms) : QStringLiteral("-")) :
        tr("No live co-op status (not in game, co-op off, or this build has no status publisher)"));
    const auto value = [&](const std::string& published, const char* env) {
        return live && !published.empty() ? published : Env(env);
    };
    identity->setText(tr("Room: %1   Team: %2   You: %3").arg(
        Q(value(status.room, "EDEN_NX2_OOT_COOP_ROOM")), Q(value(status.team, "EDEN_NX2_OOT_COOP_TEAM")),
        Q(value(status.name, "EDEN_NX2_OOT_COOP_NAME"))));
    local->setText(live ? tr("You: %1, %2").arg(status.local_child ? tr("child") : tr("adult"),
        status.local_area.empty() ? QStringLiteral("-") : Q(status.local_area)) : QString{});
    const auto trigger = Gui::TeleportTrigger(Env);
    std::vector<std::string> ids;
    for (const auto& peer : status.peers) ids.push_back(peer.id);
    if (peer_ids != ids) {
        peer_ids = ids;
        peers->setRowCount(0);
        peers->setRowCount(static_cast<int>(ids.size()));
        for (size_t row = 0; row < ids.size(); ++row) {
            for (int col = 0; col < 5; ++col) peers->setItem(static_cast<int>(row), col, new QTableWidgetItem);
            auto* teleport = new QPushButton(tr("Teleport to"), peers);
            peers->setCellWidget(static_cast<int>(row), 5, teleport);
            connect(teleport, &QPushButton::clicked, this, [this, target = ids[row].substr(0, 8)] {
                message->setText(Gui::RequestTeleport(Gui::TeleportTrigger(Env), target) ?
                    tr("Teleport requested") : tr("Could not write teleport request"));
            });
        }
    }
    const bool relay_up = status.relay_state == "connected";
    for (size_t i = 0; i < status.peers.size(); ++i) {
        const auto& peer = status.peers[i];
        const int row = static_cast<int>(i);
        peers->item(row, 0)->setText(Q(peer.name));
        // The presence book keeps peers "awaiting" across a relay gap (reconnect reconciliation), so a
        // peer is only online here while our own relay link is connected (C17: Disconnect showed Yes).
        const bool online = live && relay_up && peer.online;
        peers->item(row, 1)->setText(live ? (online ? tr("Yes") : tr("No")) : tr("Unknown"));
        peers->item(row, 2)->setText(peer.has_pose ? (peer.child ? tr("child") : tr("adult")) : QStringLiteral("-"));
        peers->item(row, 3)->setText(peer.loading ? tr("Loading") : Q(peer.area));
        peers->item(row, 4)->setText(Seen(now, peer.seen_ms));
        // A stale publisher cannot confirm online peers; do not send requests based on stale rows.
        peers->cellWidget(row, 5)->setEnabled(online && status.teleport && !trigger.empty());
    }
    const bool disconnected = Gui::DisconnectRequested(Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir));
    disconnect_button->setEnabled(live && !disconnected);
    connect_button->setEnabled(disconnected);
    reconnect_button->setEnabled(system.IsPoweredOn()); // restarting needs a running game
    const auto room_path = Gui::RoomTrigger(Env);
    const QSignalBlocker block(room_window);
    room_window->setEnabled(!room_path.empty());
    room_window->setChecked(Gui::RoomWindowShown(room_path));
    auto seed = Env("EDEN_NX2_OOT_COOP_SEED");
    randomizer[0]->setText(seed.empty() || seed == "0" ? tr("0 (vanilla)") : Q(seed));
    randomizer[1]->setText(live && !status.mod_hash.empty() ? Q(status.mod_hash) : tr("not reported by this build"));
    randomizer[2]->setText(Q(value(status.policy, "EDEN_NX2_OOT_COOP_POLICY")));
    QStringList mods;
    const auto title = system.IsPoweredOn() ? system.GetApplicationProcessProgramID() : 0;
    if (Gui::CoopTitle(title)) {
        const auto path = Common::FS::GetEdenPath(Common::FS::EdenPath::LoadDir) / fmt::format("{:016X}", title);
        const auto disabled = Settings::values.disabled_addons.find(title);
        try {
            for (const auto& folder : Core::OotCoop::Ledger::EnabledModFolders(path,
                     disabled == Settings::values.disabled_addons.end() ? std::vector<std::string>{} : disabled->second))
                mods << Q(Common::FS::PathToUTF8String(folder.filename()));
        } catch (const std::filesystem::filesystem_error&) {
            mods << tr("mod folders unavailable");
        }
    }
    randomizer[3]->setText(mods.join(QLatin1Char{','}));
    for (size_t i = 0; i < copy_values.size(); ++i) copy_values[i] = randomizer[i]->text();
    copy_values[0] = Q(seed.empty() ? "0" : seed);
}
