// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

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

} // namespace AudioCore::Nx2AudioOptions
