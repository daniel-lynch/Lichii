// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string_view>

namespace AudioCore::Nx2AudioOptions {

inline bool SharedManagersEnabled() {
    const char* value = std::getenv("EDEN_NX2_AUDIO_SHARED_MANAGERS");
    return !value || std::string_view{value} != "0";
}

inline bool AsyncMemoryPoolControlEnabled() {
    const char* value = std::getenv("EDEN_NX2_AUDIO_ASYNC_POOL");
    return !value || std::string_view{value} != "0";
}

inline bool StereoOutputEnabled() {
    const char* value = std::getenv("EDEN_NX2_AUDIO_STEREO");
    return value && std::string_view{value} == "1";
}

inline bool RampFixEnabled() {
    const char* value = std::getenv("EDEN_NX2_AUDIO_RAMP_FIX");
    return value && std::string_view{value} == "1";
}

inline float CenterGain() {
    constexpr float default_gain = 0.596f;
    const char* value = std::getenv("EDEN_NX2_AUDIO_CENTER_GAIN");
    if (!value) {
        return default_gain;
    }
    char* end{};
    const float gain = std::strtof(value, &end);
    if (end == value || *end != '\0' || !std::isfinite(gain)) {
        return default_gain;
    }
    return std::clamp(gain, 0.0f, 2.0f);
}

} // namespace AudioCore::Nx2AudioOptions
