// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <filesystem>
#include <fstream>

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/vk_frame_dump.h"

namespace {
using Vulkan::FrameDumpSchedule;
using Step = FrameDumpSchedule::Step;
} // Anonymous namespace

TEST_CASE("Frame dump schedule is idle until configured or armed", "[frame_dump_schedule]") {
    FrameDumpSchedule schedule;
    REQUIRE(schedule.Tick(100.0) == Step::None);
    REQUIRE_FALSE(schedule.Capturing());
    REQUIRE_FALSE(Vulkan::RecordingSampledTextures());
}

TEST_CASE("Frame dump schedule records sampled textures for an armed sampled capture",
          "[frame_dump_schedule]") {
    FrameDumpSchedule schedule;
    REQUIRE(schedule.Arm(true, false, 7));
    REQUIRE_FALSE(Vulkan::RecordingSampledTextures());
    // The pipelines read the flag while the captured frame is drawn, between Start and Finish.
    REQUIRE(schedule.Tick(0.0) == Step::Start);
    REQUIRE(schedule.Capturing());
    REQUIRE(schedule.Sampled());
    REQUIRE_FALSE(schedule.Buffers());
    REQUIRE(schedule.CaptureId() == 7);
    REQUIRE(Vulkan::RecordingSampledTextures());
    REQUIRE_FALSE(schedule.Arm(false, false, 8));
    REQUIRE(schedule.Tick(0.0) == Step::Finish);
    REQUIRE_FALSE(schedule.Capturing());
    REQUIRE_FALSE(Vulkan::RecordingSampledTextures());
    REQUIRE(schedule.Tick(0.0) == Step::None);

    // The next request decides again: this one does not read sampled textures back.
    REQUIRE(schedule.Arm(false, false, 8));
    REQUIRE(schedule.Tick(0.0) == Step::Start);
    REQUIRE(schedule.CaptureId() == 8);
    REQUIRE_FALSE(Vulkan::RecordingSampledTextures());
    REQUIRE(schedule.Tick(0.0) == Step::Finish);

    REQUIRE(schedule.Arm(true, false, 9));
    REQUIRE(schedule.Tick(0.0) == Step::Start);
    REQUIRE(Vulkan::RecordingSampledTextures());
    schedule.Abort();
    REQUIRE_FALSE(schedule.Capturing());
    REQUIRE_FALSE(Vulkan::RecordingSampledTextures());
    REQUIRE(schedule.Tick(0.0) == Step::None);
}

TEST_CASE("Frame dump schedule records buffers independently of sampled textures",
          "[frame_dump_schedule]") {
    FrameDumpSchedule schedule;
    REQUIRE(schedule.Arm(false, true, 3));
    REQUIRE(schedule.Tick(0.0) == Step::Start);
    REQUIRE(schedule.Buffers());
    REQUIRE_FALSE(schedule.Sampled());
    // Buffers come from the buffer cache's bindings, not from the pipelines' sampled list.
    REQUIRE_FALSE(Vulkan::RecordingSampledTextures());
    REQUIRE(schedule.Tick(0.0) == Step::Finish);
    REQUIRE(schedule.Arm(true, true, 4));
    REQUIRE(schedule.Tick(0.0) == Step::Start);
    REQUIRE(schedule.Buffers());
    REQUIRE(Vulkan::RecordingSampledTextures());
    REQUIRE(schedule.Tick(0.0) == Step::Finish);
}

TEST_CASE("Frame dump schedule follows EDEN_NX2_FRAME_DUMP_AT", "[frame_dump_schedule]") {
    FrameDumpSchedule by_frame;
    by_frame.Configure("3f", {}, true);
    REQUIRE(by_frame.Tick(1000.0) == Step::None);
    REQUIRE(by_frame.Tick(1000.0) == Step::None);
    REQUIRE(by_frame.Tick(1000.0) == Step::Start);
    REQUIRE(by_frame.Frame() == 3);
    REQUIRE(by_frame.Sampled());
    REQUIRE(by_frame.Buffers());
    REQUIRE(Vulkan::RecordingSampledTextures());
    REQUIRE(by_frame.Tick(1000.0) == Step::Finish);
    REQUIRE(by_frame.Tick(1000.0) == Step::None);

    FrameDumpSchedule by_seconds;
    by_seconds.Configure("0.5", {}, false);
    REQUIRE(by_seconds.Tick(0.4) == Step::None);
    REQUIRE(by_seconds.Tick(0.6) == Step::Start);
    REQUIRE_FALSE(Vulkan::RecordingSampledTextures());
    REQUIRE(by_seconds.Tick(0.7) == Step::Finish);

    const auto directory = std::filesystem::temp_directory_path() / "frame_dump_schedule_test";
    std::filesystem::create_directories(directory);
    const auto trigger = directory / "capture.now";
    std::filesystem::remove(trigger);
    FrameDumpSchedule by_file;
    by_file.Configure("file", trigger, false);
    for (int frame = 1; frame <= 60; ++frame) {
        REQUIRE(by_file.Tick(1000.0) == Step::None);
    }
    std::ofstream{trigger}.put('\n');
    for (int frame = 61; frame < 90; ++frame) {
        REQUIRE(by_file.Tick(1000.0) == Step::None);
    }
    REQUIRE(by_file.Tick(1000.0) == Step::Start);
    REQUIRE(by_file.Frame() == 90);
    std::filesystem::remove_all(directory);
}

TEST_CASE("Frame dump schedule captures EDEN_NX2_FRAME_DUMP_COUNT consecutive frames",
          "[frame_dump_schedule]") {
    FrameDumpSchedule single;
    single.Configure("2f", {}, false);
    REQUIRE(single.Tick(0.0) == Step::None);
    REQUIRE(single.Tick(0.0) == Step::Start);
    REQUIRE(single.FrameSubdirectory().empty());
    REQUIRE(single.LastOfBurst());
    REQUIRE(single.Tick(0.0) == Step::Finish);

    FrameDumpSchedule burst;
    burst.Configure("2f", {}, true, 3);
    REQUIRE(burst.Tick(0.0) == Step::None);
    REQUIRE(burst.Tick(0.0) == Step::Start);
    REQUIRE(burst.Frame() == 2);
    REQUIRE(burst.FrameSubdirectory() == "frame-1");
    REQUIRE_FALSE(burst.LastOfBurst());
    REQUIRE(Vulkan::RecordingSampledTextures());
    REQUIRE(burst.Tick(0.0) == Step::Next);
    REQUIRE(burst.Frame() == 3);
    REQUIRE(burst.FrameSubdirectory() == "frame-2");
    REQUIRE(burst.Capturing());
    REQUIRE(Vulkan::RecordingSampledTextures());
    REQUIRE(burst.Tick(0.0) == Step::Next);
    REQUIRE(burst.FrameSubdirectory() == "frame-3");
    REQUIRE(burst.LastOfBurst());
    REQUIRE(burst.Tick(0.0) == Step::Finish);
    REQUIRE_FALSE(burst.Capturing());
    REQUIRE_FALSE(Vulkan::RecordingSampledTextures());
    REQUIRE(burst.Tick(0.0) == Step::None);

    // The count is clamped to 1-8.
    FrameDumpSchedule clamped;
    clamped.Configure("1f", {}, false, 50);
    REQUIRE(clamped.Tick(0.0) == Step::Start);
    for (int frame = 2; frame <= 8; ++frame) {
        REQUIRE(clamped.Tick(0.0) == Step::Next);
    }
    REQUIRE(clamped.FrameSubdirectory() == "frame-8");
    REQUIRE(clamped.Tick(0.0) == Step::Finish);
}

TEST_CASE("Frame dump schedule bursts for the debug server", "[frame_dump_schedule]") {
    FrameDumpSchedule schedule;
    REQUIRE(schedule.Arm(false, false, 11, 2));
    REQUIRE(schedule.Tick(0.0) == Step::Start);
    REQUIRE(schedule.CaptureId() == 11);
    REQUIRE(schedule.FrameSubdirectory() == "frame-1");
    REQUIRE_FALSE(schedule.Arm(false, false, 12, 2));
    REQUIRE(schedule.Tick(0.0) == Step::Next);
    REQUIRE(schedule.FrameSubdirectory() == "frame-2");
    REQUIRE(schedule.LastOfBurst());
    REQUIRE(schedule.Tick(0.0) == Step::Finish);
    REQUIRE(schedule.CaptureId() == 11);
    REQUIRE(schedule.Tick(0.0) == Step::None);

    // A later single-frame request is not a burst, whatever the last one was.
    REQUIRE(schedule.Arm(false, false, 12));
    REQUIRE(schedule.Tick(0.0) == Step::Start);
    REQUIRE(schedule.FrameSubdirectory().empty());
    REQUIRE(schedule.Tick(0.0) == Step::Finish);
}

TEST_CASE("Frame dump schedule re-arms on the trigger file", "[frame_dump_schedule]") {
    const auto directory = std::filesystem::temp_directory_path() / "frame_dump_schedule_rearm";
    std::filesystem::create_directories(directory);
    const auto trigger = directory / "capture.now";
    std::filesystem::remove(trigger);
    FrameDumpSchedule schedule;
    schedule.Configure("file", trigger, false, 2);
    REQUIRE(schedule.Rearms());
    std::ofstream{trigger}.put('\n');
    for (int frame = 1; frame < 30; ++frame) {
        REQUIRE(schedule.Tick(0.0) == Step::None);
    }
    REQUIRE(schedule.Tick(0.0) == Step::Start);
    // The trigger is consumed when the burst starts.
    REQUIRE_FALSE(std::filesystem::exists(trigger));
    REQUIRE(schedule.FrameSubdirectory() == "frame-1");
    REQUIRE(schedule.Tick(0.0) == Step::Next);
    REQUIRE(schedule.FrameSubdirectory() == "frame-2");
    REQUIRE(schedule.Tick(0.0) == Step::Finish);
    // Re-armed: idle until the file comes back, then the second burst goes to burst-2/.
    for (int frame = 33; frame <= 90; ++frame) {
        REQUIRE(schedule.Tick(0.0) == Step::None);
    }
    std::ofstream{trigger}.put('\n');
    for (int frame = 91; frame < 120; ++frame) {
        REQUIRE(schedule.Tick(0.0) == Step::None);
    }
    REQUIRE(schedule.Tick(0.0) == Step::Start);
    REQUIRE(schedule.Frame() == 120);
    REQUIRE(schedule.FrameSubdirectory() == std::filesystem::path{"burst-2"} / "frame-1");
    REQUIRE(schedule.Tick(0.0) == Step::Next);
    REQUIRE(schedule.FrameSubdirectory() == std::filesystem::path{"burst-2"} / "frame-2");
    REQUIRE(schedule.Tick(0.0) == Step::Finish);

    // A debug server capture in between does not count as a burst and keeps the file armed.
    REQUIRE(schedule.Arm(false, false, 3, 1));
    REQUIRE(schedule.Tick(0.0) == Step::Start);
    REQUIRE(schedule.FrameSubdirectory().empty());
    REQUIRE(schedule.Tick(0.0) == Step::Finish);
    REQUIRE(schedule.Tick(0.0) == Step::None);
    std::ofstream{trigger}.put('\n');
    Step step = Step::None;
    for (int tick = 0; tick < 30 && step == Step::None; ++tick) {
        step = schedule.Tick(0.0);
    }
    REQUIRE(step == Step::Start);
    REQUIRE(schedule.CaptureId() == 0);
    REQUIRE(schedule.FrameSubdirectory() == std::filesystem::path{"burst-3"} / "frame-1");
    std::filesystem::remove_all(directory);
}
