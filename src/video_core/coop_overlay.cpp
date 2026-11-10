// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <cmath>
#include <string_view>

#include <fmt/format.h>

#include "video_core/coop_overlay.h"
#include "video_core/coop_overlay_font.h"
#include "video_core/coop_overlay_sans.h"

namespace VideoCore::CoopOverlay {
namespace {
namespace Font = CoopOverlayFont;
namespace Sans = CoopOverlaySans;
using Core::OotCoop::Ux::OverlayFrame;
using Core::OotCoop::Ux::Tone;

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
// Toasts and name tags (pills are opaque in both the frame copy and screenshots).
constexpr Rgb PillBack{16, 19, 28};
constexpr Rgb PillEdge{74, 84, 110};
constexpr Rgb Well{40, 46, 64};
constexpr Rgb NameColour{128, 212, 255};
constexpr Rgb PlainColour{220, 223, 232};
constexpr Rgb ItemColour{255, 214, 102};
constexpr Rgb MutedColour{150, 156, 172};
constexpr Rgb TagNear{245, 245, 245};
constexpr Rgb TagFar{200, 204, 214};
constexpr size_t MaxToastChars = 160;

Rgb ToneColour(Tone tone) {
    switch (tone) {
    case Tone::Name: return NameColour;
    case Tone::Item: return ItemColour;
    case Tone::Muted: return MutedColour;
    case Tone::Plain: break;
    }
    return PlainColour;
}

const Sans::Face& FaceOf(bool small) {
    return small ? Sans::Small : Sans::Body;
}

const Sans::Glyph& GlyphOf(const Sans::Face& face, char c) {
    u32 u = static_cast<u8>(c);
    if (u < Sans::First || u >= Sans::First + Sans::Count) u = '?';
    return face.glyphs[u - Sans::First];
}

u32 SansCoverage(const Sans::Face& face, const Sans::Glyph& glyph, u32 x, u32 y) {
    const u8 byte = face.data[glyph.offset + y * ((glyph.width + 1u) / 2u) + x / 2];
    return ((x & 1) ? (byte >> 4) : (byte & 0xF)) * 17u;
}

void Put(Box& box, s32 x, s32 y, Rgb colour, u32 a) {
    if (x < 0 || y < 0 || x >= static_cast<s32>(box.width) || y >= static_cast<s32>(box.height)) return;
    u8* p = &box.rgba[(size_t{static_cast<u32>(y)} * box.width + static_cast<u32>(x)) * 4];
    if (!p[3]) return; // outside the shape: never paint (it would not be copied anyway)
    p[0] = static_cast<u8>((colour.r * a + p[0] * (255 - a)) / 255);
    p[1] = static_cast<u8>((colour.g * a + p[1] * (255 - a)) / 255);
    p[2] = static_cast<u8>((colour.b * a + p[2] * (255 - a)) / 255);
}

/// Proportional text over the box's existing pixels; returns the pen x after the text.
s32 DrawText(Box& box, s32 x, s32 top, std::string_view text, Rgb colour, u32 scale, bool small) {
    const auto& face = FaceOf(small);
    for (const char c : text) {
        const auto& glyph = GlyphOf(face, c);
        for (u32 gy = 0; gy < face.height; ++gy) {
            for (u32 gx = 0; gx < glyph.width; ++gx) {
                const u32 a = SansCoverage(face, glyph, gx, gy);
                if (!a) continue;
                for (u32 sy = 0; sy < scale; ++sy)
                    for (u32 sx = 0; sx < scale; ++sx)
                        Put(box, x + (glyph.left + static_cast<s32>(gx)) * static_cast<s32>(scale) + static_cast<s32>(sx),
                            top + static_cast<s32>(gy * scale + sy), colour, a);
            }
        }
        x += static_cast<s32>(glyph.advance * scale);
    }
    return x;
}

/// A rounded rectangle (border `edge`, fill `fill`), shaped: alpha 0 outside. Corners are not
/// anti-aliased because the frame copy cannot blend.
Box Pill(u32 w, u32 h, u32 radius, u32 border, Rgb fill, Rgb edge) {
    Box box;
    box.width = w, box.height = h, box.shaped = true;
    box.rgba.assign(size_t{w} * h * 4, 0);
    radius = std::min({radius, w / 2, h / 2});
    const auto inside = [&](float px, float py, float inset) {
        const float r = std::max(0.0f, static_cast<float>(radius) - inset);
        if (px < inset || py < inset || px > static_cast<float>(w) - inset || py > static_cast<float>(h) - inset)
            return false;
        const float cx = std::clamp(px, inset + r, static_cast<float>(w) - inset - r);
        const float cy = std::clamp(py, inset + r, static_cast<float>(h) - inset - r);
        return (px - cx) * (px - cx) + (py - cy) * (py - cy) <= r * r;
    };
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            const float px = static_cast<float>(x) + 0.5f, py = static_cast<float>(y) + 0.5f;
            if (!inside(px, py, 0.0f)) continue;
            const Rgb c = inside(px, py, static_cast<float>(border)) ? fill : edge;
            u8* p = &box.rgba[(size_t{y} * w + x) * 4];
            p[0] = c.r, p[1] = c.g, p[2] = c.b, p[3] = 255;
        }
    }
    return box;
}

void Disc(Box& box, s32 cx, s32 cy, u32 r, Rgb colour) {
    const s32 rr = static_cast<s32>(r);
    for (s32 y = -rr; y < rr; ++y)
        for (s32 x = -rr; x < rr; ++x) {
            const float fx = static_cast<float>(x) + 0.5f, fy = static_cast<float>(y) + 0.5f;
            if (fx * fx + fy * fy <= static_cast<float>(r * r)) Put(box, cx + x, cy + y, colour, 255);
        }
}

/// Blend an RGBA (straight alpha) square of `cell` pixels into the box at (x, y), resampled to
/// `size` pixels (nearest: integer or near-integer ratios only in practice).
void Blit(Box& box, s32 x, s32 y, const std::vector<u8>& icon, u32 cell, u32 size) {
    if (!cell || icon.size() < size_t{cell} * cell * 4) return;
    for (u32 dy = 0; dy < size; ++dy) {
        for (u32 dx = 0; dx < size; ++dx) {
            // Box filter over the source footprint (downscale) or nearest (upscale).
            const u32 x0 = dx * cell / size, x1 = std::max(x0 + 1, (dx + 1) * cell / size);
            const u32 y0 = dy * cell / size, y1 = std::max(y0 + 1, (dy + 1) * cell / size);
            u32 r = 0, g = 0, b = 0, a = 0, n = 0;
            for (u32 sy = y0; sy < y1; ++sy)
                for (u32 sx = x0; sx < x1; ++sx) {
                    const u8* s = &icon[(size_t{sy} * cell + sx) * 4];
                    r += s[0] * s[3], g += s[1] * s[3], b += s[2] * s[3], a += s[3], ++n;
                }
            if (!a) continue;
            Put(box, x + static_cast<s32>(dx), y + static_cast<s32>(dy),
                {static_cast<u8>(r / a), static_cast<u8>(g / a), static_cast<u8>(b / a)}, a / n);
        }
    }
}

struct Span {
    size_t begin, end;
};
/// Line breaks for `text` (Body face): at most `max_lines` spans no wider than `max_width`.
/// The last span is marked truncated (the caller appends "...") when text remains.
std::vector<Span> Breaks(std::string_view text, u32 max_width, u32 scale, size_t max_lines, bool& truncated) {
    std::vector<Span> lines;
    truncated = false;
    size_t at = 0;
    const auto width = [&](size_t b, size_t e) { return TextWidth(text.substr(b, e - b), scale); };
    while (at < text.size() && lines.size() < max_lines) {
        while (at < text.size() && text[at] == ' ') ++at;
        if (at >= text.size()) break;
        if (width(at, text.size()) <= max_width) {
            lines.push_back({at, text.size()});
            at = text.size();
            break;
        }
        // The last allowed line keeps room for the "..." that follows it.
        const u32 ellipsis = TextWidth("...", scale);
        const u32 limit = lines.size() + 1 == max_lines ? (max_width > ellipsis ? max_width - ellipsis : 0)
                                                         : max_width;
        size_t end = at, last_space = std::string_view::npos;
        while (end < text.size() && width(at, end + 1) <= limit) {
            if (text[end] == ' ') last_space = end;
            ++end;
        }
        if (last_space != std::string_view::npos && last_space > at && end < text.size() && text[end] != ' ')
            end = last_space;
        if (end == at) end = at + 1; // a single glyph wider than the line: take it anyway
        lines.push_back({at, end});
        at = end;
    }
    while (at < text.size() && text[at] == ' ') ++at;
    truncated = at < text.size();
    return lines;
}

u32 Coverage(char c, u32 x, u32 y) {
    const u32 index = static_cast<u8>(c) >= Font::First && static_cast<u8>(c) < Font::First + Font::Count
                          ? static_cast<u8>(c) - Font::First
                          : '?' - Font::First;
    const u8 byte = Font::Data[(index * Font::Height + y) * Font::RowBytes + x / 2];
    const u32 nibble = (x & 1) ? (byte >> 4) : (byte & 0xF);
    return nibble * 17; // 0..255
}

/// A text box: border, backing, the text anti-aliased over the backing.
Box TextBox(std::string_view text, Rgb colour, u32 scale, size_t limit = MaxChars) {
    if (text.size() > limit) text = text.substr(0, limit);
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
    const bool shaped = box.shaped;
    box = {x0, y0, w, h, std::move(out)};
    box.shaped = shaped;
    return true;
}
Box ToastBox(const Core::OotCoop::Ux::Toast& toast, const Core::OotCoop::Ux::IconPack* icons, u32 s,
             u32 max_text) {
    // Flatten the runs (and " xN") into text plus a tone per byte.
    std::string text;
    std::vector<Tone> tones;
    const auto add = [&](std::string_view t, Tone tone) {
        for (const char c : t) {
            if (text.size() >= MaxToastChars) return;
            text.push_back(c);
            tones.push_back(tone);
        }
    };
    for (const auto& run : toast.runs) add(run.text, run.tone);
    if (toast.count > 1) add(fmt::format(" x{}", toast.count), Tone::Item);
    bool truncated = false;
    auto lines = Breaks(text, max_text, s, 2, truncated);
    if (lines.empty()) lines.push_back({0, 0});
    // Icon: the actor's, else the wallet delta's.
    std::vector<u8> icon;
    u32 cell = 0;
    const u32 icon_size = 40 * s;
    if (icons) {
        auto index = toast.icon.empty() ? std::nullopt : icons->Find(toast.icon);
        if (!index && toast.wallet) index = icons->Wallet(toast.wallet);
        if (const auto* atlas = index ? icons->Best(icon_size) : nullptr) {
            icon = icons->Icon(*atlas, *index);
            cell = atlas->cell;
        }
    }
    const auto& face = FaceOf(false);
    const u32 pitch = 28 * s;
    const u32 h = lines.size() == 1 ? 52 * s : static_cast<u32>(lines.size()) * pitch + 24 * s;
    const u32 left = cell ? 60 * s : 22 * s;
    u32 text_w = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        auto line = std::string_view{text}.substr(lines[i].begin, lines[i].end - lines[i].begin);
        text_w = std::max(text_w, TextWidth(line, s) + (truncated && i + 1 == lines.size() ? TextWidth("...", s) : 0));
    }
    const u32 w = left + text_w + 22 * s;
    Box box = Pill(w, h, lines.size() == 1 ? h / 2 : 20 * s, s, PillBack, PillEdge);
    if (cell) {
        Disc(box, static_cast<s32>(26 * s), static_cast<s32>(h / 2), 22 * s, Well);
        Blit(box, static_cast<s32>(6 * s), static_cast<s32>((h - icon_size) / 2), icon, cell, icon_size);
    }
    s32 top = lines.size() == 1 ? static_cast<s32>((h - face.height * s) / 2) : static_cast<s32>(12 * s);
    for (size_t i = 0; i < lines.size(); ++i) {
        s32 x = static_cast<s32>(left);
        for (size_t c = lines[i].begin; c < lines[i].end; ++c)
            x = DrawText(box, x, top, std::string_view{&text[c], 1}, ToneColour(tones[c]), s, false);
        if (truncated && i + 1 == lines.size()) DrawText(box, x, top, "...", MutedColour, s, false);
        top += static_cast<s32>(pitch);
    }
    return box;
}

Box MoreBox(size_t more, u32 s) {
    const auto text = fmt::format("+{} more", more);
    const auto& face = FaceOf(true);
    const u32 h = 34 * s, w = TextWidth(text, s, true) + 32 * s;
    Box box = Pill(w, h, h / 2, s, PillBack, PillEdge);
    DrawText(box, static_cast<s32>(16 * s), static_cast<s32>((h - face.height * s) / 2), text, MutedColour, s, true);
    return box;
}

/// A name tag: pill with a peer-colour dot and the name, a caret below pointing at the head.
/// From FarTag on: one size smaller, dimmer, with the distance.
Box TagBox(const Core::OotCoop::Ux::Tag& tag, u32 s) {
    const bool far = tag.distance >= FarTag;
    const auto& face = FaceOf(far);
    const std::string dist = far ? fmt::format("  {}m", static_cast<int>(std::lround(std::min(tag.distance, 9999.0f)))) : "";
    const u32 h = (far ? 28 : 36) * s, r = (far ? 4 : 5) * s, cx = (far ? 13 : 16) * s;
    const u32 tx = cx + r + 7 * s;
    const std::string_view name = std::string_view{tag.text}.substr(0, 24);
    const u32 w = tx + TextWidth(name, s, far) + (far ? TextWidth(dist, s, true) : 0) + (far ? 12 : 14) * s;
    const u32 caret_h = 8 * s, caret_w = (far ? 10 : 14) * s;
    Box pill = Pill(w, h, h / 2, s, PillBack, PillEdge);
    Disc(pill, static_cast<s32>(cx), static_cast<s32>(h / 2), r, NameColour);
    const s32 x = DrawText(pill, static_cast<s32>(tx), static_cast<s32>((h - face.height * s) / 2), name,
                           far ? TagFar : TagNear, s, far);
    if (far) {
        const auto& small = FaceOf(true);
        DrawText(pill, x, static_cast<s32>((h - small.height * s) / 2), dist, MutedColour, s, true);
    }
    Box box;
    box.width = w, box.height = h + caret_h - s, box.shaped = true;
    box.rgba.assign(size_t{box.width} * box.height * 4, 0);
    std::copy(pill.rgba.begin(), pill.rgba.end(), box.rgba.begin());
    // The caret overlaps the pill's bottom border row so the two read as one shape.
    for (u32 y = 0; y < caret_h; ++y) {
        const float half = static_cast<float>(caret_w) * 0.5f * (1.0f - static_cast<float>(y) / static_cast<float>(caret_h));
        for (u32 xx = 0; xx < caret_w; ++xx) {
            const float d = std::abs(static_cast<float>(xx) + 0.5f - static_cast<float>(caret_w) * 0.5f);
            if (d > half) continue;
            const u32 bx = w / 2 - caret_w / 2 + xx, by = h - s + y;
            if (bx >= box.width || by >= box.height) continue;
            u8* p = &box.rgba[(size_t{by} * box.width + bx) * 4];
            p[0] = PillEdge.r, p[1] = PillEdge.g, p[2] = PillEdge.b, p[3] = 255;
        }
    }
    return box;
}
} // namespace

std::vector<Rect> Runs(const Box& box) {
    std::vector<Rect> out;
    if (!box.shaped) {
        out.push_back({0, 0, static_cast<s32>(box.width), static_cast<s32>(box.height)});
        return out;
    }
    std::vector<std::pair<s32, s32>> previous;
    std::vector<size_t> open; // index into out for each segment of `previous`
    for (u32 y = 0; y < box.height; ++y) {
        std::vector<std::pair<s32, s32>> segments;
        for (u32 x = 0; x < box.width;) {
            if (!box.rgba[(size_t{y} * box.width + x) * 4 + 3]) { ++x; continue; }
            const u32 start = x;
            while (x < box.width && box.rgba[(size_t{y} * box.width + x) * 4 + 3]) ++x;
            segments.emplace_back(static_cast<s32>(start), static_cast<s32>(x));
        }
        if (segments == previous && !segments.empty()) {
            for (const size_t i : open) ++out[i].bottom;
            continue;
        }
        open.clear();
        for (const auto& [x0, x1] : segments) {
            open.push_back(out.size());
            out.push_back({x0, static_cast<s32>(y), x1, static_cast<s32>(y) + 1});
        }
        previous = std::move(segments);
    }
    return out;
}

u32 TextWidth(std::string_view text, u32 scale, bool small) {
    const auto& face = FaceOf(small);
    u32 w = 0;
    for (const char c : text) w += GlyphOf(face, c).advance;
    return w * scale;
}

std::vector<std::string> Wrap(std::string_view text, u32 max_width, u32 scale, size_t max_lines) {
    bool truncated = false;
    std::vector<std::string> out;
    for (const auto& span : Breaks(text, max_width, scale, max_lines, truncated))
        out.emplace_back(text.substr(span.begin, span.end - span.begin));
    if (truncated && !out.empty()) out.back() += "...";
    return out;
}

ToastPlace PlaceToasts(Rect screen, u32 scale) {
    const s32 sw = screen.right - screen.left, sh = screen.bottom - screen.top;
    ToastPlace place;
    // Right-aligned, first toast a quarter down: clear of the game's item HUD (upper right,
    // ending near 22% of the height) and of the hearts/room panel (upper left).
    place.right = screen.right - static_cast<s32>(std::lround(0.025 * sw));
    place.top = screen.top + static_cast<s32>(std::lround(0.25 * sh));
    place.max_text = std::max<u32>(240 * scale, static_cast<u32>(std::lround(0.30 * sw)));
    return place;
}

std::shared_ptr<const Box> PanelCache::Get(const Core::OotCoop::Ux::Panel& panel, u32 scale,
                                          Rect screen, u32 width, u32 height) {
    if (box && generation == panel.generation && last_scale == scale && last_screen == screen &&
        last_width == width && last_height == height) return box;
    generation = panel.generation;
    last_scale = scale; last_screen = screen; last_width = width; last_height = height;
    box.reset();
    if (panel.lines.empty() || !scale || scale > 16) return {};
    size_t chars = 0;
    for (const auto& line : panel.lines) chars = std::max(chars, std::min<size_t>(56, line.size()));
    const size_t rows = std::min<size_t>(11, panel.lines.size());
    Box result;
    result.x = screen.left + static_cast<s32>(16 * scale);
    result.y = screen.top + static_cast<s32>(16 * scale);
    result.width = static_cast<u32>(chars) * Font::Width * scale + 16 * scale;
    result.height = static_cast<u32>(rows) * (Font::Height + 6) * scale;
    result.rgba.reserve(size_t{result.width} * result.height * 4);
    for (size_t i = 0; i < rows; ++i) {
        std::string text = panel.lines[i].substr(0, 56);
        text.resize(chars, ' ');
        const auto row = TextBox(text, ToastText, scale, 56);
        result.rgba.insert(result.rgba.end(), row.rgba.begin(), row.rgba.end());
    }
    const Rect bounds{std::max(0, screen.left), std::max(0, screen.top),
                      std::min(screen.right, static_cast<s32>(width)),
                      std::min(screen.bottom, static_cast<s32>(height))};
    if (!Clip(result, bounds)) return {};
    static std::atomic<u64> next_id{};
    result.cache_id = ++next_id;
    box = std::make_shared<const Box>(std::move(result));
    return box;
}

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
    // Panel first: fixed upload offset also lets each Vulkan slot reuse its packed bytes.
    if (frame.panel) {
        thread_local PanelCache cache;
        if (const auto panel = cache.Get(*frame.panel, scale, screen, width, height)) boxes.push_back(*panel);
    }
    constexpr std::array<Rgb, 4> colours{{{100, 220, 255}, {255, 190, 80}, {180, 255, 120}, {240, 130, 230}}};
    for (const auto& marker : frame.markers) {
        if (!std::isfinite(marker.at.x) || !std::isfinite(marker.at.y) ||
            marker.at.x < 0 || marker.at.x > 1 || marker.at.y < 0 || marker.at.y > 1) continue;
        auto letter = TextBox(std::string(1, marker.label), colours[marker.colour % colours.size()], scale);
        // Centre the glyph in a square, with an opaque border.
        const u32 side = std::max(letter.width, letter.height);
        auto square = Solid(0, 0, side, side, Border);
        const u32 ox = (side - letter.width) / 2, oy = (side - letter.height) / 2;
        for (u32 row = 0; row < letter.height; ++row)
            std::copy_n(letter.rgba.begin() + size_t{row} * letter.width * 4, size_t{letter.width} * 4,
                        square.rgba.begin() + (size_t{row + oy} * side + ox) * 4);
        square.x = screen.left + static_cast<s32>(marker.at.x * sw) - static_cast<s32>(side / 2);
        square.y = screen.top + static_cast<s32>(marker.at.y * sh) - static_cast<s32>(side / 2);
        place(std::move(square));
    }
    // Name tags before toasts, so a toast covers a tag.
    for (const auto& tag : frame.tags) {
        const s32 px = screen.left + static_cast<s32>(tag.at.x * static_cast<float>(sw));
        const s32 py = screen.top + static_cast<s32>(tag.at.y * static_cast<float>(sh));
        if (tag.marker) {
            const s32 arm = static_cast<s32>(8 * scale), t = static_cast<s32>(scale);
            place(Solid(px - arm, py - t, static_cast<u32>(2 * arm + 1), static_cast<u32>(2 * t + 1), Marker));
            place(Solid(px - t, py - arm, static_cast<u32>(2 * t + 1), static_cast<u32>(2 * arm + 1), Marker));
            continue;
        }
        auto box = TagBox(tag, scale);
        box.x = px - static_cast<s32>(box.width / 2);
        box.y = py - static_cast<s32>(box.height) - static_cast<s32>(2 * scale);
        place(std::move(box));
    }
    const auto toasts = PlaceToasts(screen, scale);
    const s32 gap = static_cast<s32>(8 * scale);
    s32 y = toasts.top;
    for (const auto& toast : frame.toasts) {
        auto box = ToastBox(toast, frame.icons.get(), scale, toasts.max_text);
        box.x = toasts.right - static_cast<s32>(box.width);
        box.y = y;
        y += static_cast<s32>(box.height) + gap;
        place(std::move(box));
    }
    if (frame.more && !frame.toasts.empty()) {
        auto box = MoreBox(frame.more, scale);
        box.x = toasts.right - static_cast<s32>(box.width);
        box.y = y;
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
