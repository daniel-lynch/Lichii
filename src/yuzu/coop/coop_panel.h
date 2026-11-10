// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <string>
#include <vector>
#include <QWidget>
#include "common/common_types.h"
namespace Core { class System; }
class QAction;
class QCheckBox;
class QHideEvent;
class QLabel;
class QPushButton;
class QShowEvent;
class QTableWidget;
class QTimer;

class CoopPanel : public QWidget {
    Q_OBJECT
public:
    explicit CoopPanel(Core::System& system, QWidget* parent = nullptr);
    QAction* toggleViewAction();
signals:
    void ReconnectRequested();
protected:
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
private:
    void Refresh();
    // Field-test hook (EDEN_NX2_OOT_COOP_GUI_SHOTS=<dir>): grabs and a command file, see coop_panel.cpp.
    void TestHook();
    Core::System& system;
    QAction* action;
    QTimer* timer;
    QLabel *header, *identity, *local, *message;
    QPushButton *resync_button{}, *copy_all_button{}, *reconnect_button{}, *disconnect_button{}, *connect_button{};
    QString shots;
    int shot_count{};
    u64 shot_serial{};
    QCheckBox* room_window;
    QTableWidget* peers;
    std::vector<std::string> peer_ids;
    std::array<QLabel*, 4> randomizer;
    std::array<QString, 4> copy_values;
};
