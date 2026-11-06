// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/buffer_cache/usage_tracker.h"

TEST_CASE("Usage tracker marks every 64-byte unit a range touches", "[video_core][usage]") {
    SECTION("A range under 64 bytes marks only its own unit") {
        VideoCommon::UsageTracker tracker(64 * 1024);
        tracker.Track(0x100, 16);
        CHECK(tracker.IsUsed(0x100, 1));
        CHECK(tracker.IsUsed(0x13f, 1));
        CHECK_FALSE(tracker.IsUsed(0x140, 64));
        CHECK_FALSE(tracker.IsUsed(0x0c0, 64));
    }
    SECTION("A partly used last unit is marked") {
        VideoCommon::UsageTracker tracker(64 * 1024);
        tracker.Track(0x200, 0x48);
        CHECK(tracker.IsUsed(0x240, 4));
        CHECK_FALSE(tracker.IsUsed(0x280, 4));
    }
    SECTION("A range ending in its first page is checked from its offset only") {
        VideoCommon::UsageTracker tracker(64 * 1024);
        tracker.Track(0x0, 64);
        // Rechecking the end page from its start saw unit 0 as used.
        CHECK_FALSE(tracker.IsUsed(0x100, 0x40));
        tracker.Track(0x1000, 64);
        CHECK(tracker.IsUsed(0xfc0, 0x80));
    }
    SECTION("A range crossing pages marks both ends") {
        VideoCommon::UsageTracker tracker(64 * 1024);
        tracker.Track(0xfe0, 0x40);
        CHECK(tracker.IsUsed(0xfc0, 1));
        CHECK(tracker.IsUsed(0x1000, 1));
        CHECK_FALSE(tracker.IsUsed(0x1040, 0x40));
    }
}
