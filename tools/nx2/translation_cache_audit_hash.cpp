// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "common/cityhash.h"
#include "video_core/renderer_vulkan/fixed_pipeline_state.h"

// Read-only audit helper, built separately from the emulator. See translation_cache_audit.py.
extern "C" void hash128(const char* data, size_t length, u64* result) {
    const auto hash = Common::CityHash128(data, length);
    result[0] = hash[0];
    result[1] = hash[1];
}

extern "C" size_t graphics_size() {
    return sizeof(std::array<u64, 6>) + sizeof(Vulkan::FixedPipelineState);
}
