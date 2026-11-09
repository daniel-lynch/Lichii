// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <string_view>

#include "video_core/coop_overlay.h"
#include "video_core/coop_overlay_font.h"

namespace VideoCore::CoopOverlay {
namespace {
namespace Font = CoopOverlayFont;
using Core::OotCoop::Ux::OverlayFrame;

struct Rgb {
    u8 r, g, b;
};
constexpr Rgb Backing{18, 20, 30};
constexpr Rgb Border{90, 100, 130};
constexpr Rgb ToastText{245, 245, 245};
constexpr Rgb TagText{255, 226, 110};
constexpr Rgb Marker{255, 0, 255};
constexpr size_t MaxChars = 48;
// The backing is drawn at this alpha where blending exists (screenshots); the frame copy is opaque.
constexpr u8 BackingAlpha = 217;

u32 Coverage(char c, u32 x, u32 y) {
    const u32 index = static_cast<u8>(c) >= Font::First && static_cast<u8>(c) < Font::First + Font::Count
                          ? static_cast<u8>(c) - Font::First
                          : '?' - Font::First;
    const u8 byte = Font::Data[(index * Font::Height + y) * Font::RowBytes + x / 2];
    const u32 nibble = (x & 1) ? (byte >> 4) : (byte & 0xF);
    return nibble * 17; // 0..255
}

/// A text box: border, backing, the text anti-aliased over the backing.
Box TextBox(std::string_view text, Rgb colour, u32 scale) {
    if (text.size() > MaxChars) text = text.substr(0, MaxChars);
    const u32 pad_x = 8 * scale, pad_y = 3 * scale;
    Box box;
    box.width = static_cast<u32>(text.size()) * Font::Width * scale + 2 * pad_x;
    box.height = Font::Height * scale + 2 * pad_y;
    box.rgba.resize(size_t{box.width} * box.height * 4);
    for (u32 y = 0; y < box.height; ++y) {
        for (u32 x = 0; x < box.width; ++x) {
            const bool edge = x < scale || y < scale || x >= box.width - scale || y >= box.height - scale;
            const Rgb base = edge ? Border : Backing;
            u8* p = &box.rgba[(size_t{y} * box.width + x) * 4];
            p[0] = base.r, p[1] = base.g, p[2] = base.b, p[3] = edge ? 255 : BackingAlpha;
        }
    }
    for (size_t i = 0; i < text.size(); ++i) {
        for (u32 gy = 0; gy < Font::Height; ++gy) {
            for (u32 gx = 0; gx < Font::Width; ++gx) {
                const u32 a = Coverage(text[i], gx, gy);
                if (!a) continue;
                for (u32 sy = 0; sy < scale; ++sy) {
                    for (u32 sx = 0; sx < scale; ++sx) {
                        const u32 x = pad_x + static_cast<u32>(i) * Font::Width * scale + gx * scale + sx;
                        const u32 y = pad_y + gy * scale + sy;
                        u8* p = &box.rgba[(size_t{y} * box.width + x) * 4];
                        p[0] = static_cast<u8>((colour.r * a + p[0] * (255 - a)) / 255);
                        p[1] = static_cast<u8>((colour.g * a + p[1] * (255 - a)) / 255);
                        p[2] = static_cast<u8>((colour.b * a + p[2] * (255 - a)) / 255);
                        p[3] = static_cast<u8>(std::max<u32>(p[3], a));
                    }
                }
            }
        }
    }
    return box;
}

Box Solid(s32 x, s32 y, u32 w, u32 h, Rgb colour) {
    Box box{x, y, w, h, std::vector<u8>(size_t{w} * h * 4)};
    for (size_t i = 0; i < size_t{w} * h; ++i) {
        box.rgba[i * 4] = colour.r, box.rgba[i * 4 + 1] = colour.g, box.rgba[i * 4 + 2] = colour.b;
        box.rgba[i * 4 + 3] = 255;
    }
    return box;
}

/// Clip to [0, width) x [0, height): nothing when the box is entirely outside.
bool Clip(Box& box, Rect bounds) {
    const s32 x0 = std::max(box.x, bounds.left), y0 = std::max(box.y, bounds.top);
    const s32 x1 = std::min(box.x + static_cast<s32>(box.width), bounds.right);
    const s32 y1 = std::min(box.y + static_cast<s32>(box.height), bounds.bottom);
    if (x1 <= x0 || y1 <= y0) return false;
    if (x0 == box.x && y0 == box.y && x1 - x0 == static_cast<s32>(box.width) &&
        y1 - y0 == static_cast<s32>(box.height))
        return true;
    const u32 w = static_cast<u32>(x1 - x0), h = static_cast<u32>(y1 - y0);
    std::vector<u8> out(size_t{w} * h * 4);
    for (u32 y = 0; y < h; ++y) {
        const size_t from = (size_t{static_cast<u32>(y0 - box.y) + y} * box.width +
                             static_cast<u32>(x0 - box.x)) * 4;
        std::copy_n(box.rgba.begin() + static_cast<std::ptrdiff_t>(from), size_t{w} * 4,
                    out.begin() + static_cast<std::ptrdiff_t>(size_t{y} * w * 4));
    }
    box = {x0, y0, w, h, std::move(out)};
    return true;
}
} // namespace

u32 Scale(u32 screen_height) {
    return std::max<u32>(1, screen_height / 1080);
}

std::vector<Box> Layout(const OverlayFrame& frame, Rect screen, u32 width, u32 height) {
    std::vector<Box> boxes;
    Rect bounds{std::max(screen.left, 0), std::max(screen.top, 0),
                std::min(screen.right, static_cast<s32>(width)),
                std::min(screen.bottom, static_cast<s32>(height))};
    if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) return boxes;
    const u32 sw = static_cast<u32>(screen.right - screen.left);
    const u32 sh = static_cast<u32>(screen.bottom - screen.top);
    const u32 scale = Scale(sh);
    const auto place = [&](Box box) {
        if (Clip(box, bounds)) boxes.push_back(std::move(box));
    };
    // Name tags first, so a toast covers a tag and never the other way round.
    for (const auto& tag : frame.tags) {
        const s32 px = screen.left + static_cast<s32>(tag.at.x * static_cast<float>(sw));
        const s32 py = screen.top + static_cast<s32>(tag.at.y * static_cast<float>(sh));
        if (tag.marker) {
            const s32 arm = static_cast<s32>(8 * scale), t = static_cast<s32>(scale);
            place(Solid(px - arm, py - t, static_cast<u32>(2 * arm + 1), static_cast<u32>(2 * t + 1), Marker));
            place(Solid(px - t, py - arm, static_cast<u32>(2 * t + 1), static_cast<u32>(2 * arm + 1), Marker));
            continue;
        }
        auto box = TextBox(tag.text, TagText, scale);
        box.x = px - static_cast<s32>(box.width / 2);
        box.y = py - static_cast<s32>(box.height) - static_cast<s32>(4 * scale);
        place(std::move(box));
    }
    const s32 margin = static_cast<s32>(16 * scale), gap = static_cast<s32>(6 * scale);
    s32 y = screen.top + margin;
    for (const auto& text : frame.toasts) {
        auto box = TextBox(text, ToastText, scale);
        box.x = screen.right - margin - static_cast<s32>(box.width);
        box.y = y;
        y += static_cast<s32>(box.height) + gap;
        place(std::move(box));
    }
    return boxes;
}

void BlendBgra(std::span<u8> pixels, u32 width, u32 height, const std::vector<Box>& boxes) {
    for (const auto& box : boxes) {
        for (u32 y = 0; y < box.height; ++y) {
            const s32 ty = box.y + static_cast<s32>(y);
            if (ty < 0 || ty >= static_cast<s32>(height)) continue;
            for (u32 x = 0; x < box.width; ++x) {
                const s32 tx = box.x + static_cast<s32>(x);
                if (tx < 0 || tx >= static_cast<s32>(width)) continue;
                const size_t at = (size_t{static_cast<u32>(ty)} * width + static_cast<u32>(tx)) * 4;
                if (at + 4 > pixels.size()) continue;
                const u8* s = &box.rgba[(size_t{y} * box.width + x) * 4];
                const u32 a = s[3];
                u8* d = &pixels[at];
                d[0] = static_cast<u8>((s[2] * a + d[0] * (255 - a)) / 255);
                d[1] = static_cast<u8>((s[1] * a + d[1] * (255 - a)) / 255);
                d[2] = static_cast<u8>((s[0] * a + d[2] * (255 - a)) / 255);
            }
        }
    }
}

} // namespace VideoCore::CoopOverlay
