// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The co-op checks overlay as one opaque box (core/arm/dynarmic/oot_coop_checks.h): a framed list on
// the right edge of the game rectangle, below the toast column, one colour per row style. Layout()
// in coop_overlay.cpp places and clips it like every other overlay box.

#include <memory>

#include "core/arm/dynarmic/oot_coop_checks.h"
#include "video_core/coop_overlay.h"

namespace VideoCore::CoopOverlay {

/// The panel rasterised and positioned for `screen` (not yet clipped); nothing for an empty panel.
[[nodiscard]] std::shared_ptr<const Box> ChecksBox(const Core::OotCoop::Checks::Panel& panel, u32 scale,
                                                   Rect screen);

/// Re-rasterises only when the panel generation, scale or game rectangle changes.
class ChecksCache {
public:
    std::shared_ptr<const Box> Get(const Core::OotCoop::Checks::Panel& panel, u32 scale, Rect screen);

private:
    std::shared_ptr<const Box> box;
    u64 generation{};
    u32 last_scale{};
    Rect last_screen;
};

} // namespace VideoCore::CoopOverlay
