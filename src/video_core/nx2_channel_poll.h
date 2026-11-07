// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <charconv>
#include <cstdlib>
#include <cstring>
#include <system_error>

#include "common/common_types.h"

// EDEN_NX2_CHANNEL_POLL_US=<microseconds>: how long the GPU thread sleeps between polls of a
// channel suspended on a semaphore acquire, once the first 500 us of yielding have passed.
// Default 50 (upstream PR 446). 1000 is the earlier 1 ms poll, with the thread's default timer
// slack; 0 keeps yielding (a busy poll of one core). Values above 100000 are clamped.
namespace VideoCommon::Nx2ChannelPoll {

constexpr u32 DefaultMicros = 50;
constexpr u32 LegacyMicros = 1000;
constexpr u32 MaxMicros = 100000;

/// The interval for a switch value (null when unset); malformed values fall back to the default.
[[nodiscard]] inline u32 ParseMicros(const char* value) {
    if (value == nullptr || value[0] == '\0') {
        return DefaultMicros;
    }
    const char* const end = value + std::strlen(value);
    u64 micros = 0;
    const auto [ptr, error] = std::from_chars(value, end, micros);
    if (error == std::errc::result_out_of_range) {
        return MaxMicros;
    }
    if (error != std::errc{} || ptr != end) {
        return DefaultMicros;
    }
    return micros > MaxMicros ? MaxMicros : static_cast<u32>(micros);
}

/// Sub-millisecond sleeps need the thread's timer slack lowered (Linux adds up to 50 us).
[[nodiscard]] constexpr bool WantsLowTimerSlack(u32 micros) {
    return micros != 0 && micros < LegacyMicros;
}

[[nodiscard]] inline u32 Micros() {
    static const u32 micros = ParseMicros(std::getenv("EDEN_NX2_CHANNEL_POLL_US"));
    return micros;
}

} // namespace VideoCommon::Nx2ChannelPoll
