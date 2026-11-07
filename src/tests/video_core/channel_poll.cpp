// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/nx2_channel_poll.h"

namespace Poll = VideoCommon::Nx2ChannelPoll;

TEST_CASE("Channel poll interval defaults to 50 us", "[video_core][nx2_channel_poll]") {
    CHECK(Poll::ParseMicros(nullptr) == 50);
    CHECK(Poll::ParseMicros("") == 50);
    CHECK(Poll::ParseMicros("abc") == 50);
    CHECK(Poll::ParseMicros("-5") == 50);
    CHECK(Poll::ParseMicros("20us") == 50);
}

TEST_CASE("Channel poll interval reads microseconds", "[video_core][nx2_channel_poll]") {
    CHECK(Poll::ParseMicros("1000") == 1000);
    CHECK(Poll::ParseMicros("200") == 200);
    CHECK(Poll::ParseMicros("0") == 0);
    CHECK(Poll::ParseMicros("100001") == Poll::MaxMicros);
    CHECK(Poll::ParseMicros("99999999999999999999999") == Poll::MaxMicros);
}

TEST_CASE("Only sub-millisecond sleeps lower the timer slack", "[video_core][nx2_channel_poll]") {
    CHECK(Poll::WantsLowTimerSlack(50));
    CHECK(Poll::WantsLowTimerSlack(999));
    CHECK_FALSE(Poll::WantsLowTimerSlack(1000));
    CHECK_FALSE(Poll::WantsLowTimerSlack(0));
}
