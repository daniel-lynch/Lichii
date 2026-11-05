// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>

#include <catch2/catch_test_macros.hpp>

#include "core/hle/service/nx2_realtime_step.h"

namespace {
using namespace std::chrono_literals;
using namespace Service::Nx2RealtimeStep;
constexpr auto Start = Clock::time_point{10s};

std::chrono::nanoseconds Period(double fps) {
    return std::chrono::nanoseconds{static_cast<long long>(1e9 / fps)};
}

// Feeds `count` presents at `fps`; returns the last f written (if any) and advances `now`.
std::optional<float> Feed(StepController& c, Clock::time_point& now, double fps, int count) {
    std::optional<float> last;
    for (int i = 0; i < count; ++i) {
        now += Period(fps);
        if (const auto f = c.OnPresent(now)) {
            last = f;
        }
    }
    return last;
}

bool Near(double a, double b, double tolerance) {
    return std::abs(a - b) <= tolerance;
}
} // namespace

TEST_CASE("Real-time step block for f = 1 matches the game's constructor", "[core][nx2_realtime_step]") {
    constexpr Block b = DeriveBlock(1.0f);
    STATIC_REQUIRE(b.main[0] == 1.0f);
    STATIC_REQUIRE(b.main[1] == 1.0f);
    STATIC_REQUIRE(b.main[3] == 60.0f);
    STATIC_REQUIRE(b.base == 1.0f);
    // The constructor stores 0x3C888889 (0.016666668f) at +0x08 and +0x18.
    REQUIRE(std::bit_cast<std::uint32_t>(b.main[2]) == 0x3C888889u);
    REQUIRE(std::bit_cast<std::uint32_t>(b.step_seconds) == 0x3C888889u);
}

TEST_CASE("Real-time step block derivation", "[core][nx2_realtime_step]") {
    const float f = 60.0f / 39.0f;
    const Block b = DeriveBlock(f);
    REQUIRE(b.main[0] == f);
    REQUIRE(Near(b.main[1], 39.0 / 60.0, 1e-6));
    REQUIRE(Near(b.main[2], 1.0 / 39.0, 1e-7));
    REQUIRE(Near(b.main[3], 39.0, 1e-4));
    REQUIRE(b.base == f);
    REQUIRE(b.step_seconds == b.main[2]);
    // f / 60 per present at 39 presents/s is one real second per second.
    REQUIRE(Near(b.step_seconds * 39.0, 1.0, 1e-5));
}

TEST_CASE("Real-time step factor clamps", "[core][nx2_realtime_step]") {
    REQUIRE(StepController::FactorForFps(60.0) == 1.0f);
    REQUIRE(StepController::FactorForFps(120.0) == 1.0f); // fps clamp 60
    REQUIRE(StepController::FactorForFps(30.0) == 2.0f);
    REQUIRE(StepController::FactorForFps(20.0) == 3.0f);
    REQUIRE(StepController::FactorForFps(5.0) == 3.0f); // fps clamp 20
    REQUIRE(Near(StepController::FactorForFps(39.0), 60.0 / 39.0, 1e-6));
}

TEST_CASE("Real-time step waits for the warm-up and stays at f = 1 at 60 fps", "[core][nx2_realtime_step]") {
    StepController c;
    auto now = Start;
    REQUIRE_FALSE(Feed(c, now, 60.0, 600).has_value());
    REQUIRE(c.Written() == 1.0f);
    REQUIRE(Near(c.MeasuredFps(), 60.0, 0.1));

    StepController d;
    now = Start;
    // Nothing during the warm-up even at 39 fps...
    REQUIRE_FALSE(Feed(d, now, 39.0, StepController::WarmupPresents - 1).has_value());
    // ...then the first write.
    const auto f = Feed(d, now, 39.0, 1);
    REQUIRE(f.has_value());
    REQUIRE(Near(*f, 60.0 / 39.0, 1e-3));
}

TEST_CASE("Real-time step EMA follows a rate change within about a second", "[core][nx2_realtime_step]") {
    StepController c;
    auto now = Start;
    Feed(c, now, 39.0, 120);
    REQUIRE(Near(c.Written(), 60.0 / 39.0, 0.02));
    // 0.25 s into a change to 55 fps: not there yet (time constant 0.5 s).
    Feed(c, now, 55.0, 14);
    REQUIRE(c.MeasuredFps() < 52.0);
    // 1.5 s in (three time constants): within 2 % hysteresis of 60/55.
    Feed(c, now, 55.0, 70);
    REQUIRE(Near(c.Written(), 60.0 / 55.0, 0.03));
}

TEST_CASE("Real-time step hysteresis ignores jitter under 2 %", "[core][nx2_realtime_step]") {
    StepController c;
    auto now = Start;
    Feed(c, now, 40.0, 120);
    const float settled = c.Written();
    int writes = 0;
    // Alternate 39.6 / 40.4 fps (+-1 %): the EMA never moves f past 2 %.
    for (int i = 0; i < 600; ++i) {
        now += Period(i % 2 ? 39.6 : 40.4);
        writes += c.OnPresent(now).has_value() ? 1 : 0;
    }
    REQUIRE(writes == 0);
    REQUIRE(c.Written() == settled);
    // A real move (40 -> 36 fps, 11 %) is written.
    REQUIRE(Feed(c, now, 36.0, 120).has_value());
    REQUIRE(Near(c.Written(), 60.0 / 36.0, 0.04));
}

TEST_CASE("Real-time step clamps a load stall instead of following it", "[core][nx2_realtime_step]") {
    StepController c;
    auto now = Start;
    Feed(c, now, 39.0, 120);
    now += 3s; // one 3 s hitch
    c.OnPresent(now);
    REQUIRE(c.MeasuredFps() >= StepController::MinFps);
    REQUIRE(c.Written() <= StepController::MaxF);
    Feed(c, now, 39.0, 120);
    REQUIRE(Near(c.Written(), 60.0 / 39.0, 0.04));
}

TEST_CASE("Real-time step reset starts over from f = 1", "[core][nx2_realtime_step]") {
    StepController c;
    auto now = Start;
    Feed(c, now, 30.0, 120);
    REQUIRE(c.Written() > 1.9f);
    c.Reset();
    REQUIRE(c.Written() == 1.0f);
    REQUIRE_FALSE(c.OnPresent(now + 1s).has_value());
}

TEST_CASE("Real-time step never goes below f = 1 above 60 presents/s", "[core][nx2_realtime_step]") {
    // With the frame limiter off the guest can present faster than 60: f stays 1 (no write), the
    // game runs fast as it would on hardware without vsync.
    StepController c;
    auto now = Start;
    REQUIRE_FALSE(Feed(c, now, 90.0, 600).has_value());
    REQUIRE(c.Written() == 1.0f);
    // Coming down from 30 fps to 120 fps: written back to exactly 1, never below.
    Feed(c, now, 30.0, 120);
    REQUIRE(c.Written() > 1.9f);
    const auto f = Feed(c, now, 120.0, 600);
    REQUIRE(f.has_value());
    REQUIRE(*f == 1.0f);
}
