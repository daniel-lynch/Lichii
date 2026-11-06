// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdlib>

#include "common/common_types.h"

namespace Service::Nx2OotTime {

// Ocarina of Time's frame loop (main+0x6e6be0) computes each frame's delta as 60 / target fps and
// never looks at the clock, so everything runs in slow motion below that rate. The built-in patch
// in patch_manager.cpp makes the loop load the delta from a word in the zeros after .text, and the
// present path writes the measured frame time there.
inline constexpr u64 ProgramId = 0x0400486024DEC000ULL;
inline constexpr u64 DeltaOffset = 0x3C461E0;
inline constexpr u64 LoadDeltaOffset = 0x3C461E4;
inline constexpr u64 ReturnOffset = 0x3C461E8;
inline constexpr u64 HookOffset = 0x6E6CC4;
inline constexpr u32 HookInstruction = 0x14D57D48; // b 0x3c461e4
inline constexpr u32 LoadDeltaInstruction = 0x1CFFFFE8; // ldr s8, 0x3c461e0
inline constexpr u32 ReturnInstruction = 0x172A82B8;    // b 0x6e6cc8

/// EDEN_NX2_REAL_SPEED=0 stops the present path writing the measured delta: the patched loop then
/// reads the 1.0 the built-in patch starts the word with, the game's fixed step at 60 fps. The
/// delta is still computed. EDEN_NX2_OOT_REAL_TIME=0 is Anthony's spelling of the
/// same switch, accepted so either fork's run scripts work: either one set to 0 turns the write
/// off. The Add-Ons entry "Real-time speed (built-in)" turns the code patch itself off.
[[nodiscard]] inline bool WriteEnabled() {
    static const bool enabled = [] {
        const auto off = [](const char* name) {
            const char* value = std::getenv(name);
            return value != nullptr && value[0] == '0';
        };
        return !off("EDEN_NX2_REAL_SPEED") && !off("EDEN_NX2_OOT_REAL_TIME");
    }();
    return enabled;
}

inline constexpr float DefaultSnapBelow = 1.1f;

/// EDEN_NX2_OOT_REAL_TIME_SNAP=<float>, read once: deltas below it are delivered as the fixed 1.0.
/// Default 1.1 (54.5 fps), clamped to [1.0, 1.5].
inline float SnapFromEnv() {
    static const float snap = [] {
        const char* value = std::getenv("EDEN_NX2_OOT_REAL_TIME_SNAP");
        if (value == nullptr) {
            return DefaultSnapBelow;
        }
        char* end = nullptr;
        const float parsed = std::strtof(value, &end);
        // NaN or no number: the default.
        return end == value || !(parsed == parsed) ? DefaultSnapBelow
                                                    : std::clamp(parsed, 1.0f, 1.5f);
    }();
    return snap;
}

/// The game's delta (1.0 = one 60th of a second) for the frame that follows a present.
class FrameDelta {
public:
    using Clock = std::chrono::steady_clock;

    FrameDelta() = default;
    explicit FrameDelta(float snap) : snap_below{snap} {}

    [[nodiscard]] float SnapBelow() const {
        return snap_below;
    }

    [[nodiscard]] float Update(Clock::time_point now) {
        if (last != Clock::time_point{}) {
            // A pause or a shader stall isn't time the game should catch up on.
            const float seconds = std::chrono::duration<float>(now - last).count();
            average_seconds += ((std::min)(seconds, MaxFrameSeconds) - average_seconds) * Smoothing;
        }
        last = now;
        const float delta = std::clamp(average_seconds * 60.0f, 1.0f, MaxDelta);
        // Near 60 fps, keep the game's own fixed step instead of following timing jitter.
        return delta < snap_below ? 1.0f : delta;
    }

private:
    static constexpr float MaxFrameSeconds = 0.1f;
    static constexpr float Smoothing = 0.125f;
    static constexpr float MaxDelta = 4.0f;

    float snap_below = DefaultSnapBelow;
    Clock::time_point last{};
    float average_seconds = 1.0f / 60.0f;
};

} // namespace Service::Nx2OotTime
