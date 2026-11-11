// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>

#include "video_core/coop_checks_overlay.h"
#include "video_core/coop_overlay_font.h"

namespace VideoCore::CoopOverlay {
namespace {
namespace Font = CoopOverlayFont;
using Core::OotCoop::Checks::Style;

struct Rgb {
    u8 r, g, b;
};
constexpr Rgb Backing{18, 20, 30};
constexpr Rgb Border{90, 100, 130};
constexpr u8 BackingAlpha = 217; // same as the other overlay boxes (blended only in screenshots)
constexpr size_t MaxChars = 56;
constexpr size_t MaxRows = 24;

Rgb Colour(Style style) {
    switch (style) {
    case Style::Title: return {255, 226, 110};     // the name-tag yellow
    case Style::Open: return {245, 245, 245};      // the toast white
    case Style::Done: return {120, 200, 120};      // green, checked
    case Style::Untracked: return {150, 175, 215}; // blue-grey: no flag to read
    case Style::Note: return {165, 165, 175};
    }
    return {245, 245, 245};
}

u32 Coverage(char c, u32 x, u32 y) {
    const auto u = static_cast<u8>(c);
    const u32 index = u >= Font::First && u < Font::First + Font::Count ? u - Font::First : '?' - Font::First;
    const u8 byte = Font::Data[(index * Font::Height + y) * Font::RowBytes + x / 2];
    return ((x & 1) ? (byte >> 4) : (byte & 0xF)) * 17;
}
} // namespace

std::shared_ptr<const Box> ChecksBox(const Core::OotCoop::Checks::Panel& panel, u32 scale, Rect screen) {
    if (panel.rows.empty() || !scale || scale > 16) return {};
    const size_t rows = std::min(panel.rows.size(), MaxRows);
    size_t chars = 0;
    for (size_t i = 0; i < rows; ++i) chars = std::max(chars, std::min(panel.rows[i].text.size(), MaxChars));
    const u32 pad_x = 8 * scale, pad_y = 4 * scale, line = (Font::Height + 3) * scale;
    auto box = std::make_shared<Box>();
    box->width = static_cast<u32>(chars) * Font::Width * scale + 2 * pad_x;
    box->height = static_cast<u32>(rows) * line + 2 * pad_y;
    box->rgba.resize(size_t{box->width} * box->height * 4);
    for (u32 y = 0; y < box->height; ++y) {
        for (u32 x = 0; x < box->width; ++x) {
            const bool edge = x < scale || y < scale || x >= box->width - scale || y >= box->height - scale;
            // A rule under the title row.
            const bool rule = rows > 1 && y >= pad_y + line - scale && y < pad_y + line && x >= pad_x &&
                              x < box->width - pad_x;
            const Rgb base = edge || rule ? Border : Backing;
            u8* p = &box->rgba[(size_t{y} * box->width + x) * 4];
            p[0] = base.r, p[1] = base.g, p[2] = base.b, p[3] = edge || rule ? 255 : BackingAlpha;
        }
    }
    for (size_t r = 0; r < rows; ++r) {
        const auto& row = panel.rows[r];
        const Rgb colour = Colour(row.style);
        const size_t n = std::min(row.text.size(), MaxChars);
        for (size_t i = 0; i < n; ++i) {
            for (u32 gy = 0; gy < Font::Height; ++gy) {
                for (u32 gx = 0; gx < Font::Width; ++gx) {
                    const u32 a = Coverage(row.text[i], gx, gy);
                    if (!a) continue;
                    for (u32 sy = 0; sy < scale; ++sy) {
                        for (u32 sx = 0; sx < scale; ++sx) {
                            const u32 x = pad_x + static_cast<u32>(i) * Font::Width * scale + gx * scale + sx;
                            const u32 y = pad_y + static_cast<u32>(r) * line + gy * scale + sy;
                            u8* p = &box->rgba[(size_t{y} * box->width + x) * 4];
                            p[0] = static_cast<u8>((colour.r * a + p[0] * (255 - a)) / 255);
                            p[1] = static_cast<u8>((colour.g * a + p[1] * (255 - a)) / 255);
                            p[2] = static_cast<u8>((colour.b * a + p[2] * (255 - a)) / 255);
                            p[3] = static_cast<u8>(std::max<u32>(p[3], a));
                        }
                    }
                }
            }
        }
    }
    // Right edge of the game rectangle, below the toast column (three toasts at most).
    const s32 margin = static_cast<s32>(16 * scale);
    const s32 sh = screen.bottom - screen.top;
    box->x = screen.right - margin - static_cast<s32>(box->width);
    box->y = screen.top + std::max<s32>(sh * 24 / 100, static_cast<s32>(150 * scale));
    return box;
}

std::shared_ptr<const Box> ChecksCache::Get(const Core::OotCoop::Checks::Panel& panel, u32 scale, Rect screen) {
    if (box && generation == panel.generation && last_scale == scale && last_screen == screen) return box;
    generation = panel.generation;
    last_scale = scale;
    last_screen = screen;
    box = ChecksBox(panel, scale, screen);
    return box;
}

} // namespace VideoCore::CoopOverlay
