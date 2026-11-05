// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>

namespace Core {
class System;
}
namespace Kernel {
class KProcess;
}

// Real-time step for Ocarina of Time (0400486024DEC000, main build id E0F3CCFA...).
//
// The game advances a constant 1/60 s per presented frame with no catch-up, so at 39 presents/s
// game time runs at 0.65x real time. Its time object (the pointer at main+0x4E94B20) holds the
// step multiplier f and the blocks derived from it; nothing in the game writes them after
// construction, so rewriting them sticks. With EDEN_NX2_REALTIME_STEP=1 (or the oot_realtime_step
// setting) each present measures the present rate and sets f = 60 / fps. docs/nx2/
// oot-realtime-step.md has the layout, the risks and the measurements.
namespace Service::Nx2RealtimeStep {

using Clock = std::chrono::steady_clock;

// What the game keeps for one f: +0x00..+0x0c {f, 1/f, f/60, 60/f}, +0x10 f, +0x18 f/60 s.
// The per-core blocks at +0x20 + core * 0x30 are the same four floats, re-derived by the game.
struct Block {
    std::array<float, 4> main; // +0x00
    float base;                // +0x10
    float step_seconds;        // +0x18
};

// Matches the constructor's values for f = 1 bit for bit ({1, 1, 1/60, 60}, 1, 1/60): the game
// multiplies by the float 0.016666668 and divides 60 by f the same way.
constexpr float OneSixtieth = 1.0f / 60.0f;

constexpr Block DeriveBlock(float f) {
    return Block{
        .main = {f, 1.0f / f, f * OneSixtieth, 60.0f / f},
        .base = f,
        .step_seconds = f * OneSixtieth,
    };
}

// The present-rate estimate and the f it implies. Pure: no guest memory, testable.
class StepController {
public:
    static constexpr double MinFps = 20.0;
    static constexpr double MaxFps = 60.0;
    static constexpr float MinF = 1.0f;
    static constexpr float MaxF = 3.0f;
    // EMA time constant: about the last half second of presents.
    static constexpr double TimeConstantSeconds = 0.5;
    // Rewrite only when f moves by more than this fraction.
    static constexpr double Hysteresis = 0.02;
    // Presents to see before the first write, so the estimate has settled (about 0.5 s at 39).
    static constexpr int WarmupPresents = 20;

    // One present at `now`. Returns the f to write when it should change, else nullopt.
    std::optional<float> OnPresent(Clock::time_point now) {
        if (!last_present) {
            last_present = now;
            samples = 1; // the warm-up counts presents, this one included
            return std::nullopt;
        }
        const double raw = std::chrono::duration<double>(now - *last_present).count();
        last_present = now;
        if (raw <= 0.0) {
            return std::nullopt;
        }
        // A load-screen stall of seconds would drag the estimate far down for seconds after;
        // the clamp below would cap it at 20 fps anyway, so clamp the sample there first.
        const double interval = std::clamp(raw, 1.0 / MaxFps, 1.0 / MinFps);
        const double alpha = 1.0 - std::exp(-interval / TimeConstantSeconds);
        ema_interval = ema_interval ? *ema_interval + alpha * (interval - *ema_interval) : interval;
        if (++samples < WarmupPresents) {
            return std::nullopt;
        }
        float f = FactorForFps(1.0 / *ema_interval);
        // Within the band of 1 (above ~58.8 presents/s) is 1: a held 60 with vblank jitter must
        // write the constructor's values, not 1.002.
        if (f < static_cast<float>(1.0 + Hysteresis)) {
            f = MinF;
        }
        // Settle onto a clamp exactly: with the 2 % band alone, coming back to 60 fps could stop
        // at f = 1.015 and never write 1.0 (the feature must be a no-op at a held 60).
        const bool reaches_bound = (f == MinF || f == MaxF) && static_cast<double>(f) != written;
        if (!reaches_bound && std::abs(static_cast<double>(f) - written) <= Hysteresis * written) {
            return std::nullopt;
        }
        written = f;
        return f;
    }

    double MeasuredFps() const {
        return ema_interval ? std::clamp(1.0 / *ema_interval, MinFps, MaxFps) : MaxFps;
    }

    float Written() const {
        return static_cast<float>(written);
    }

    // After a restore (f = 1 written back) or a new process: start the estimate over.
    void Reset() {
        *this = {};
    }

    static float FactorForFps(double fps) {
        const double clamped = std::clamp(fps, MinFps, MaxFps);
        return std::clamp(static_cast<float>(60.0 / clamped), MinF, MaxF);
    }

private:
    std::optional<Clock::time_point> last_present;
    std::optional<double> ema_interval;
    int samples{};
    double written{1.0}; // the game constructs the object with f = 1
};

// The guest side: finds the time object and writes the blocks. One per vi2 session (the present
// thread), so no locking.
class OotRealtimeStep {
public:
    static constexpr std::uint64_t ProgramId = 0x0400486024DEC000ULL;
    static constexpr std::uint64_t TimeObjectCell = 0x4E94B20; // main + this: the object pointer

    // Called on every present, before the reply.
    void OnPresent(Core::System& system, Kernel::KProcess* process, Clock::time_point now);
    // Writes f = 1 back if this wrote anything (setting turned off, or the session closing).
    void Restore(Core::System& system, Kernel::KProcess* process);

private:
    bool Matches(Core::System& system, Kernel::KProcess* process);
    bool Write(Kernel::KProcess* process, float f);

    StepController controller;
    std::optional<bool> matches;       // title + build id + main base, decided once
    std::uint64_t main_base{};
    Kernel::KProcess* written_process{}; // non-null while f != 1 is in guest memory
    bool engaged_logged{};
    std::uint32_t bad_object_logs{};
};

// EDEN_NX2_REALTIME_STEP=1/0 wins when set; otherwise Settings::values.oot_realtime_step.
bool Enabled();

} // namespace Service::Nx2RealtimeStep
