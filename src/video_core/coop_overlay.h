// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The co-op overlay as pixel boxes (CPU raster, embedded bitmap fonts): toasts as rounded pills
// (item icon, coloured runs) right-aligned below the game's item HUD, name tags as pills with a
// caret above their projected point, the room panel upper left. The Vulkan presenter copies the
// boxes into the composited frame after composition and before present (no blending: a shaped
// box copies only its covered rows/runs, see Runs()), so nothing reads or writes depth;
// screenshots blend the same boxes into their pixels.

#include <span>
#include <vector>

#include "common/common_types.h"
#include "core/arm/dynarmic/oot_coop_ux.h"

namespace VideoCore::CoopOverlay {

struct Rect {
    s32 left{}, top{}, right{}, bottom{};
    bool operator==(const Rect&) const = default;
};

/// One opaque box: RGBA8 (R, G, B, A in memory), row-major, already clipped to the frame.
struct Box {
    s32 x{}, y{};
    u32 width{}, height{};
    std::vector<u8> rgba;
    u64 cache_id{}; // nonzero only for an immutable rasterised panel
    bool shaped{};  // alpha 0 pixels are outside the shape: copy only the covered runs
};

/// The rectangles a shaped box covers (pixels with alpha > 0), in box coordinates: one run per
/// row segment, consecutive rows with the same segments merged. Unshaped: the whole box.
[[nodiscard]] std::vector<Rect> Runs(const Box& box);

/// Toast placement for a game rectangle `width` x `height`: right edge inset, first toast top,
/// the widest text line (proportional pixels at `scale`).
struct ToastPlace {
    s32 right{}, top{};
    u32 max_text{};
};
[[nodiscard]] ToastPlace PlaceToasts(Rect screen, u32 scale);
/// Split `text` into at most `max_lines` lines no wider than `max_width` (Body face at `scale`),
/// at spaces where possible; the last line ends in "..." if the text does not fit.
[[nodiscard]] std::vector<std::string> Wrap(std::string_view text, u32 max_width, u32 scale,
                                            size_t max_lines = 2);
/// Proportional text width (Body or Small face) in pixels at `scale`.
[[nodiscard]] u32 TextWidth(std::string_view text, u32 scale, bool small = false);
/// Name tags beyond this many world units use the compact style with a distance.
inline constexpr float FarTag = 15.0f;

class PanelCache {
public:
    std::shared_ptr<const Box> Get(const Core::OotCoop::Ux::Panel& panel, u32 scale,
                                   Rect screen, u32 width, u32 height);
private:
    std::shared_ptr<const Box> box;
    u64 generation{};
    u32 last_scale{}, last_width{}, last_height{};
    Rect last_screen;
};

/// The integer text scale for a game rectangle this tall (1 below 2160 px, 2 from 2160, ...).
[[nodiscard]] u32 Scale(u32 screen_height);
/// Boxes for `frame`; `screen` is the game rectangle inside a `width` x `height` frame.
[[nodiscard]] std::vector<Box> Layout(const Core::OotCoop::Ux::OverlayFrame& frame, Rect screen,
                                      u32 width, u32 height);
/// Blend the boxes (alpha 0.85 backing, the text itself opaque) into BGRA8 pixels, for screenshots.
void BlendBgra(std::span<u8> pixels, u32 width, u32 height, const std::vector<Box>& boxes);

} // namespace VideoCore::CoopOverlay
