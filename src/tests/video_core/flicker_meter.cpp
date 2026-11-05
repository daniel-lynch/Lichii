// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "video_core/nx2_debug_control.h"
#include "video_core/nx2_flash_trigger.h"
#include "video_core/nx2_flicker_meter.h"

using namespace VideoCore::Nx2Debug;

namespace {
std::vector<u8> Grey(u8 value) {
    std::vector<u8> rgba(size_t{ProbeWidth} * ProbeHeight * 4, value);
    for (size_t i = 3; i < rgba.size(); i += 4) {
        rgba[i] = 255;
    }
    return rgba;
}

/// Paints tile `tile` (8x6 samples) with `value`.
std::vector<u8> WithTile(std::vector<u8> rgba, u32 tile, u8 value) {
    const u32 tx = tile % TileColumns * (ProbeWidth / TileColumns);
    const u32 ty = tile / TileColumns * (ProbeHeight / TileRows);
    for (u32 y = ty; y < ty + ProbeHeight / TileRows; ++y) {
        for (u32 x = tx; x < tx + ProbeWidth / TileColumns; ++x) {
            for (u32 c = 0; c < 3; ++c) {
                rgba[(size_t{y} * ProbeWidth + x) * 4 + c] = value;
            }
        }
    }
    return rgba;
}

FrameFeatures F(const std::vector<u8>& rgba) {
    return *ComputeFeatures(rgba);
}
} // Anonymous namespace

TEST_CASE("Flicker features: centre mean, tiles, wrong sizes", "[flicker_meter]") {
    REQUIRE_FALSE(ComputeFeatures(std::vector<u8>(16)));
    const auto grey = F(Grey(100));
    REQUIRE(grey.centre_mean > 99.9);
    REQUIRE(grey.centre_mean < 100.1);
    REQUIRE(grey.tile_sd[0] < 0.01f);
    // A corner tile is outside the centre region.
    const auto corner = F(WithTile(Grey(100), 0, 200));
    REQUIRE(corner.centre_mean < 100.1);
    REQUIRE(corner.tile_mean[0] > 199.9f);
    REQUIRE(corner.digest != grey.digest);
}

TEST_CASE("A one-frame brightening over both neighbours is a flash", "[flicker_meter]") {
    FlickerMeter meter{{.flash_jump = 6, .tile_jump = 12}};
    REQUIRE_FALSE(meter.Push(1, F(Grey(100))));
    REQUIRE_FALSE(meter.Push(2, F(Grey(110))));
    const auto verdict = meter.Push(3, F(Grey(101)));
    REQUIRE(verdict);
    REQUIRE(verdict->frame == 2);
    REQUIRE(verdict->flash);
    REQUIRE_FALSE(verdict->flicker); // all tiles spike too, but a flash is not counted twice
    REQUIRE(verdict->flash_jump > 8.9);
    // A ramp (camera turning towards the sky) is not a flash.
    const auto ramp = meter.Push(4, F(Grey(130)));
    REQUIRE(ramp);
    REQUIRE_FALSE(ramp->flash);
    const auto next = meter.Push(5, F(Grey(160)));
    REQUIRE(next);
    REQUIRE_FALSE(next->flash);
    REQUIRE(next->flicker_tiles == 0);
}

TEST_CASE("A single tile spiking for one frame is flicker, not a flash", "[flicker_meter]") {
    FlickerMeter meter{{.flash_jump = 6, .tile_jump = 12}};
    meter.Push(1, F(Grey(100)));
    meter.Push(2, F(WithTile(Grey(100), 0, 160)));
    const auto verdict = meter.Push(3, F(Grey(100)));
    REQUIRE(verdict);
    REQUIRE(verdict->flicker);
    REQUIRE_FALSE(verdict->flash);
    REQUIRE(verdict->flicker_tiles == 1);
    // A dark dip is flicker too.
    meter.Push(4, F(WithTile(Grey(100), 9, 20)));
    const auto dip = meter.Push(5, F(Grey(100)));
    REQUIRE(dip);
    REQUIRE(dip->frame == 4);
    REQUIRE(dip->flicker);
}

TEST_CASE("Repeated presents are skipped and dropped probes restart the window", "[flicker_meter]") {
    FlickerMeter meter{{.flash_jump = 6, .tile_jump = 12}};
    meter.Push(1, F(Grey(100)));
    meter.Push(2, F(Grey(120)));
    REQUIRE_FALSE(meter.Push(3, F(Grey(120)))); // the same guest frame shown twice
    REQUIRE(meter.Duplicates() == 1);
    const auto verdict = meter.Push(4, F(Grey(100)));
    REQUIRE(verdict);
    REQUIRE(verdict->flash); // a two-present flash of one guest frame still counts
    REQUIRE_FALSE(meter.Push(6, F(Grey(140))));
    REQUIRE(meter.Gaps() == 1);
    REQUIRE_FALSE(meter.Push(7, F(Grey(100))));
    REQUIRE(meter.Push(8, F(Grey(90))));
}

TEST_CASE("Flash monitor windows, totals and the first-flash capture", "[flicker_meter]") {
    BeginSession();
    REQUIRE_FALSE(FlashEnabled());
    REQUIRE_FALSE(MonitorGeneration());
    const auto started = StartMonitor({.directory = "flash-monitor-test"});
    REQUIRE(FlashEnabled());
    REQUIRE_FALSE(GetFlashConfig()); // the white-fraction trigger stays off
    const u64 id = started.generation;
    const auto dark = Grey(50);
    const auto bright = Grey(200);
    ObserveMonitor(id, 1, 3, dark);
    ObserveMonitor(id, 2, 4, bright);
    ObserveMonitor(id, 3, 5, dark);
    auto request = TakeCapture();
    REQUIRE(request);
    REQUIRE(request->directory == std::filesystem::path{"flash-monitor-test"} / "first-flash");
    auto window = ReadMonitor(true);
    REQUIRE(window.window.flashes == 1);
    REQUIRE(window.window.last_flash_frame == 2);
    REQUIRE(window.capture);
    REQUIRE(window.capture->capture_frame == 6);
    ObserveMonitor(id, 4, 6, bright);
    ObserveMonitor(id, 5, 7, dark);
    REQUIRE_FALSE(TakeCapture()); // only the first flash is captured
    window = ReadMonitor(true);
    REQUIRE(window.window.flashes == 1);
    REQUIRE(window.total.flashes == 2);
    REQUIRE(window.total.frames == 5);
    ObserveMonitor(id + 1, 6, 8, bright); // a stale generation is ignored
    REQUIRE(ReadMonitor(false).window.frames == 0);
    FinishCapture(request->id, {});
    REQUIRE(ReadMonitor(false).capture->complete);
    StopMonitor();
    REQUIRE_FALSE(FlashEnabled());
    REQUIRE_FALSE(MonitorGeneration());
    EndSession();
}
