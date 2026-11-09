// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <QWidget>
#include "core/arm/dynarmic/oot_coop_gui.h"
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QSpinBox;
class QtConfig;

class ConfigureCoop : public QWidget {
public:
    explicit ConfigureCoop(u64 title_id, QtConfig* config, QWidget* parent = nullptr);
    void LoadConfiguration();
    void ApplyConfiguration();
private:
    void RefreshKit();
    QtConfig* config;
    u64 title_id;
    Core::OotCoop::Gui::Settings saved;
    QCheckBox *enabled, *tint, *ghost;
    QLineEdit *host, *room, *name, *team, *seed, *keys, *policy;
    QSpinBox* port;
    QLabel *kit_info, *policy_info, *error;
    std::array<QCheckBox*, 7> classes;
    QComboBox *quest, *teleport, *cleanup;
    bool ghost_changed{};
};
