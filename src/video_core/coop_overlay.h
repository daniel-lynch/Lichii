// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The co-op overlay as opaque pixel boxes (CPU raster, embedded bitmap font): corner toasts in the
// upper right of the game rectangle and name tags centred above their projected point. The Vulkan
// presenter copies the boxes into the composited frame after composition and before present, so
// nothing reads or writes depth; screenshots blend the same boxes into their pixels.

#include <span>
#include <vector>

#include "common/common_types.h"
#include "core/arm/dynarmic/oot_coop_ux.h"

namespace VideoCore::CoopOverlay {

struct Rect {
    s32 left{}, top{}, right{}, bottom{};
};

/// One opaque box: RGBA8 (R, G, B, A in memory), row-major, already clipped to the frame.
struct Box {
    s32 x{}, y{};
    u32 width{}, height{};
    std::vector<u8> rgba;
};

/// The integer text scale for a game rectangle this tall (1 below 2160 px, 2 from 2160, ...).
[[nodiscard]] u32 Scale(u32 screen_height);
/// Boxes for `frame`; `screen` is the game rectangle inside a `width` x `height` frame.
[[nodiscard]] std::vector<Box> Layout(const Core::OotCoop::Ux::OverlayFrame& frame, Rect screen,
                                      u32 width, u32 height);
/// Blend the boxes (alpha 0.85 backing, the text itself opaque) into BGRA8 pixels, for screenshots.
void BlendBgra(std::span<u8> pixels, u32 width, u32 height, const std::vector<Box>& boxes);

} // namespace VideoCore::CoopOverlay
