// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string_view>
#include "common/common_types.h"
class Config;
void ApplyCoopEnvironment(u64 title_id, const Config& per_game);
// Before the first boot, checks the current environment; afterwards uses the initial snapshot.
bool CoopEnvironmentIsExternal(std::string_view name);
