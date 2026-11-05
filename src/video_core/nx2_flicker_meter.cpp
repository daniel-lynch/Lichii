// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cmath>
#include "video_core/nx2_flicker_meter.h"

namespace VideoCore::Nx2Debug {

std::optional<FrameFeatures> ComputeFeatures(std::span<const u8> rgba) {
    if (rgba.size() != size_t{ProbeWidth} * ProbeHeight * 4) {
        return std::nullopt;
    }
    constexpr u32 tile_w = ProbeWidth / TileColumns;
    constexpr u32 tile_h = ProbeHeight / TileRows;
    FrameFeatures out;
    std::array<double, TileCount> sum{};
    std::array<double, TileCount> sum_sq{};
    double centre{};
    u32 centre_n{};
    u64 digest = 0xcbf29ce484222325ULL;
    for (u32 y = 0; y < ProbeHeight; ++y) {
        for (u32 x = 0; x < ProbeWidth; ++x) {
            const u8* p = &rgba[(size_t{y} * ProbeWidth + x) * 4];
            for (u32 c = 0; c < 3; ++c) {
                digest = (digest ^ p[c]) * 0x100000001b3ULL;
            }
            const double luma = (2126.0 * p[0] + 7152.0 * p[1] + 722.0 * p[2]) / 10000.0;
            const u32 tile = (y / tile_h) * TileColumns + x / tile_w;
            sum[tile] += luma;
            sum_sq[tile] += luma * luma;
            if (x >= ProbeWidth / 4 && x < ProbeWidth * 3 / 4 && y >= ProbeHeight / 4 &&
                y < ProbeHeight * 3 / 4) {
                centre += luma;
                ++centre_n;
            }
        }
    }
    constexpr double n = tile_w * tile_h;
    for (u32 t = 0; t < TileCount; ++t) {
        const double mean = sum[t] / n;
        out.tile_mean[t] = static_cast<float>(mean);
        out.tile_sd[t] = static_cast<float>(std::sqrt((std::max)(0.0, sum_sq[t] / n - mean * mean)));
    }
    out.centre_mean = centre / centre_n;
    out.digest = digest;
    return out;
}

namespace {
/// Signed one-frame spike of b against a and c: positive when b is above both, negative when
/// below both, zero otherwise.
double Spike(double a, double b, double c) {
    const double up = (std::min)(b - a, b - c);
    if (up > 0) {
        return up;
    }
    const double down = (std::min)(a - b, c - b);
    return down > 0 ? -down : 0.0;
}
} // Anonymous namespace

std::optional<FrameVerdict> FlickerMeter::Push(u64 frame, const FrameFeatures& features) {
    if (have_last && frame == last_frame + 1 && size > 0 &&
        window[size - 1].features.digest == features.digest) {
        ++duplicates;
        last_frame = frame;
        return std::nullopt;
    }
    if (have_last && frame != last_frame + 1) {
        ++gaps;
        size = 0;
    }
    have_last = true;
    last_frame = frame;
    if (size == 3) {
        window[0] = window[1];
        window[1] = window[2];
        size = 2;
    }
    window[size++] = {frame, features};
    if (size < 3) {
        return std::nullopt;
    }
    const auto& a = window[0].features;
    const auto& b = window[1].features;
    const auto& c = window[2].features;
    FrameVerdict verdict{.frame = window[1].frame};
    verdict.flash_jump = (std::min)(b.centre_mean - a.centre_mean, b.centre_mean - c.centre_mean);
    verdict.flash = verdict.flash_jump >= thresholds.flash_jump;
    for (u32 t = 0; t < TileCount; ++t) {
        const double spike = (std::max)(
            std::abs(Spike(a.tile_mean[t], b.tile_mean[t], c.tile_mean[t])),
            std::abs(Spike(a.tile_sd[t], b.tile_sd[t], c.tile_sd[t])));
        verdict.tile_jump = (std::max)(verdict.tile_jump, spike);
        verdict.flicker_tiles += spike >= thresholds.tile_jump;
    }
    verdict.flicker = verdict.flicker_tiles > 0 && !verdict.flash;
    return verdict;
}

} // namespace VideoCore::Nx2Debug
