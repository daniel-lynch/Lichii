// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <memory>
#include <optional>

#include <catch2/catch_test_macros.hpp>

#include "video_core/nx2.h"
#include "video_core/nx2_acquire_decision.h"
#include "video_core/nx2_inflight_release.h"

using VideoCommon::DecideEqualMask;
using VideoCommon::EqualMaskStep;
using VideoCommon::QueuedWordRelease;

namespace {

constexpr auto Equal = [](u32 value) { return [value](u32 current) { return current == value; }; };
constexpr auto Mask = [](u32 value) { return [value](u32 current) { return (current & value) != 0; }; };

} // Anonymous namespace

TEST_CASE("Equal and mask acquires pass on memory only when nothing queued can write the word",
          "[video_core][nx2_acquire]") {
    const std::optional<QueuedWordRelease> none;
    CHECK(DecideEqualMask(Equal(5), none, 5, false) == EqualMaskStep::Met);
    // Queued work may still change a word that meets it now: wait for that work, then read.
    CHECK(DecideEqualMask(Equal(5), none, 5, true) == EqualMaskStep::WaitForWrites);
    // Not meeting it never passes early, queued writes or not: poll again.
    CHECK(DecideEqualMask(Equal(5), none, 4, false) == EqualMaskStep::NotMet);
    CHECK(DecideEqualMask(Equal(5), none, 6, true) == EqualMaskStep::NotMet);
    CHECK(DecideEqualMask(Mask(0x4), none, 0x6, false) == EqualMaskStep::Met);
    CHECK(DecideEqualMask(Mask(0x4), none, 0x6, true) == EqualMaskStep::WaitForWrites);
    CHECK(DecideEqualMask(Mask(0x4), none, 0x3, false) == EqualMaskStep::NotMet);
    CHECK(DecideEqualMask(Mask(0x4), none, 0x0, true) == EqualMaskStep::NotMet);
}

TEST_CASE("Equal and mask acquires follow the last release queued to the word",
          "[video_core][nx2_acquire]") {
    using Q = QueuedWordRelease;
    // It meets the acquire: wait for it to run (and land) instead of passing on its word.
    CHECK(DecideEqualMask(Equal(7), std::optional<Q>{Q{7, true}}, 3, true) ==
          EqualMaskStep::WaitForWrites);
    CHECK(DecideEqualMask(Mask(0x10), std::optional<Q>{Q{0x30, true}}, 0, true) ==
          EqualMaskStep::WaitForWrites);
    // It will overwrite a word that meets the acquire now with one that doesn't.
    CHECK(DecideEqualMask(Equal(7), std::optional<Q>{Q{8, true}}, 7, false) ==
          EqualMaskStep::NotMet);
    CHECK(DecideEqualMask(Mask(0x1), std::optional<Q>{Q{0x2, true}}, 0x1, true) ==
          EqualMaskStep::NotMet);
    // A reduction or a counter report: the front can't tell what it writes.
    CHECK(DecideEqualMask(Equal(7), std::optional<Q>{Q{7, false}}, 7, false) ==
          EqualMaskStep::Drain);
    CHECK(DecideEqualMask(Mask(0x1), std::optional<Q>{Q{0, false}}, 0, true) ==
          EqualMaskStep::Drain);
}

TEST_CASE("Peeking at a mirrored semaphore word keeps it for a combined release",
          "[video_core][nx2_acquire]") {
    Tegra::Nx2MirrorShadow shadow;
    shadow.Set(0x1000, 7, 3);
    CHECK(shadow.Peek(0x1000, 3) == std::optional<u32>{7});
    // The guest word changed (a combined release on the back is writing it): Peek sees no
    // mirrored value but leaves it, Read forgets it.
    CHECK_FALSE(shadow.Peek(0x1000, 4).has_value());
    CHECK(shadow.Read(0x1000, 3) == std::optional<u32>{7});
    CHECK_FALSE(shadow.Read(0x1000, 4).has_value());
    CHECK_FALSE(shadow.Read(0x1000, 3).has_value());
}

TEST_CASE("In-flight acquires pass only on a matching release that has not landed",
          "[video_core][nx2_acquire]") {
    using VideoCommon::DecideInflight;
    using VideoCommon::InflightIssue;
    using VideoCommon::InflightIssues;
    using VideoCommon::InflightNote;
    using VideoCommon::InflightStep;
    using N = std::optional<InflightNote>;
    using I = std::optional<InflightIssue>;
    const N seven{InflightNote{7, true, 10}};
    // Queued for the back: greater-or-equal passes (it runs ahead of the acquire's channel); an
    // equal or mask acquire can't see writes queued after it, so it decides as before.
    CHECK(DecideInflight(Equal(7), true, seven, true, I{}, 0) == InflightStep::Queued);
    CHECK(DecideInflight(Equal(7), false, seven, true, I{}, 0) == InflightStep::None);
    // A payload that does not meet it, an unknown one, or no release: decide as before.
    CHECK(DecideInflight(Equal(7), false, N{InflightNote{8, true, 10}}, false, I{InflightIssue{8, 5}}, 4) == InflightStep::None);
    CHECK(DecideInflight(Equal(7), false, N{InflightNote{7, false, 10}}, false, I{InflightIssue{7, 5}}, 4) == InflightStep::None);
    CHECK(DecideInflight(Equal(7), false, N{}, false, I{InflightIssue{7, 5}}, 4) == InflightStep::None);
    // Run as an async report (issue count 5 after it): in flight until 5 completed.
    CHECK(DecideInflight(Equal(7), false, seven, false, I{InflightIssue{7, 5}}, 4) == InflightStep::Unlanded);
    CHECK(DecideInflight(Mask(0x4), false, N{InflightNote{0x6, true, 10}}, false, I{InflightIssue{0x6, 5}}, 4) == InflightStep::Unlanded);
    CHECK(DecideInflight(Equal(7), false, seven, false, I{InflightIssue{7, 5}}, 5) == InflightStep::None);
    CHECK(DecideInflight(Equal(7), false, seven, false, I{InflightIssue{7, 5}}, 9) == InflightStep::None);
    // The back's last write to the word is another one (an un-noted release): decide as before.
    CHECK(DecideInflight(Equal(7), false, seven, false, I{InflightIssue{6, 5}}, 4) == InflightStep::None);
    // No write recorded (written at once, forgotten by a reduction): memory decides.
    CHECK(DecideInflight(Equal(7), false, seven, false, I{}, 0) == InflightStep::None);
    // 16-bit counts wrap.
    CHECK(DecideInflight(Equal(7), false, seven, false, I{InflightIssue{7, 2}}, 0x1ffffu) == InflightStep::Unlanded);
    CHECK(DecideInflight(Equal(7), false, seven, false, I{InflightIssue{7, 2}}, 0x10002u) == InflightStep::None);
    // The table answers only for the word it holds, with the payload it was written.
    auto issues = std::make_unique<InflightIssues>();
    CHECK(!issues->Find(0x1000));
    issues->Wrote(0x1000, 7, 0x12345);
    REQUIRE(issues->Find(0x1000));
    CHECK(issues->Find(0x1000)->payload == 7);
    CHECK(issues->Find(0x1000)->issued_after == 0x2345);
    CHECK(!issues->Find(0x1004));
    issues->Forget(0x1000);
    CHECK(!issues->Find(0x1000));
}
