// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <optional>

#include <catch2/catch_test_macros.hpp>

#include "video_core/nx2_acquire_decision.h"

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
    // It meets the acquire and nothing sent after it can write the word: the word will hold it.
    CHECK(DecideEqualMask(Equal(7), std::optional<Q>{Q{7, true, false}}, 3, true) ==
          EqualMaskStep::MetByQueued);
    CHECK(DecideEqualMask(Mask(0x10), std::optional<Q>{Q{0x30, true, false}}, 0, true) ==
          EqualMaskStep::MetByQueued);
    // Work sent after it may write the word again.
    CHECK(DecideEqualMask(Equal(7), std::optional<Q>{Q{7, true, true}}, 3, true) ==
          EqualMaskStep::WaitForWrites);
    // It will overwrite a word that meets the acquire now with one that doesn't.
    CHECK(DecideEqualMask(Equal(7), std::optional<Q>{Q{8, true, false}}, 7, false) ==
          EqualMaskStep::NotMet);
    CHECK(DecideEqualMask(Mask(0x1), std::optional<Q>{Q{0x2, true, true}}, 0x1, true) ==
          EqualMaskStep::NotMet);
    // A reduction or a counter report: the front can't tell what it writes.
    CHECK(DecideEqualMask(Equal(7), std::optional<Q>{Q{7, false, false}}, 7, false) ==
          EqualMaskStep::Drain);
    CHECK(DecideEqualMask(Mask(0x1), std::optional<Q>{Q{0, false, true}}, 0, true) ==
          EqualMaskStep::Drain);
}
