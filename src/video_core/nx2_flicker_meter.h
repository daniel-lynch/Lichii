// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <optional>
#include <span>
#include "common/common_types.h"

/// Continuous flash/flicker statistics over the flash detector's downsampled present probe
/// (docs/nx2/debug-mcp.md, "Continuous flash and flicker statistics"). Pure CPU logic, so it is
/// unit-testable without a GPU; the Vulkan side only supplies the 64x36 RGBA8 readbacks.
namespace VideoCore::Nx2Debug {

constexpr u32 ProbeWidth = 64;
constexpr u32 ProbeHeight = 36;
/// 8x6 tiles of 8x6 samples each.
constexpr u32 TileColumns = 8;
constexpr u32 TileRows = 6;
constexpr u32 TileCount = TileColumns * TileRows;

struct FrameFeatures {
    /// Mean Rec.709 luma (0..255) of the central half of the frame (32x18 samples).
    double centre_mean{};
    /// Mean luma and luma standard deviation of each tile (0..255).
    std::array<float, TileCount> tile_mean{};
    std::array<float, TileCount> tile_sd{};
    /// FNV-1a of the RGB samples: a repeated present (same guest frame shown twice) is skipped.
    u64 digest{};
};

/// rgba holds ProbeWidth x ProbeHeight RGBA8 samples; anything else returns nullopt.
[[nodiscard]] std::optional<FrameFeatures> ComputeFeatures(std::span<const u8> rgba);

struct FlickerThresholds {
    /// A flash: the centre mean is at least this much brighter than BOTH neighbouring frames.
    double flash_jump{6.0};
    /// A flicker tile: a tile's mean or standard deviation differs from BOTH neighbours by at
    /// least this much, in the same direction (a one-frame spike or dip).
    double tile_jump{12.0};
};

/// The verdict for one frame, available once the next distinct frame arrived.
struct FrameVerdict {
    u64 frame{};
    /// min(centre(t) - centre(t-1), centre(t) - centre(t+1)); positive = a one-frame brightening.
    double flash_jump{};
    bool flash{};
    /// Tiles whose mean or deviation spiked; the largest such spike.
    u32 flicker_tiles{};
    double tile_jump{};
    /// A flicker frame: tiles spiked but the frame is not a flash.
    bool flicker{};
};

class FlickerMeter {
public:
    explicit FlickerMeter(FlickerThresholds thresholds = {}) : thresholds{thresholds} {}

    /// Feeds one probed present, in present order. A frame whose number is not the previous
    /// one plus one (a dropped probe) restarts the three-frame window. An exact repeat of the
    /// previous frame is skipped (returns nullopt, counted in duplicates()). Otherwise returns
    /// the verdict for the previous distinct frame once it has both neighbours.
    std::optional<FrameVerdict> Push(u64 frame, const FrameFeatures& features);

    [[nodiscard]] u64 Duplicates() const noexcept { return duplicates; }
    [[nodiscard]] u64 Gaps() const noexcept { return gaps; }
    [[nodiscard]] const FlickerThresholds& Thresholds() const noexcept { return thresholds; }

private:
    struct Entry {
        u64 frame;
        FrameFeatures features;
    };
    FlickerThresholds thresholds;
    std::array<Entry, 3> window{};
    u32 size{};
    u64 last_frame{};
    bool have_last{};
    u64 duplicates{};
    u64 gaps{};
};

} // namespace VideoCore::Nx2Debug
