// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "video_core/offline_shader_producer.h"

namespace VideoCommon::OfflineShaders {
/// What producing one archive compute program did.
struct ComputeOutcome {
    bool translated{};  ///< A fresh IR translation ran.
    bool resumed{};     ///< The product already held this program's module.
    bool validated{};   ///< An existing disk module passed its decode check.
    bool invalidated{}; ///< An existing disk module failed its decode check and was re-emitted.
    u64 module_bytes{}; ///< Uncompressed SPIR-V bytes emitted in this run (0 when none).
};

/// Translates one archive compute program into the store under exactly the keys the runtime
/// computes at its first dispatch: stage key slot 5 over the control section's dispatch state
/// (workgroup size, shared and local memory), and a single variant with the zero runtime digest
/// and default bindings (compute has no emission recipe). Throws std::runtime_error whose
/// message names the skip reason.
/// shared_memory_limit mirrors the runtime clamp on drivers with small shared memory (0: none).
[[nodiscard]] ComputeOutcome ProduceCompute(const Program& row, const Shader::Profile& profile,
                                            const Shader::HostTranslateInfo& host,
                                            const TranslationCache::Digest& config,
                                            TranslationCache::Store& store,
                                            u32 shared_memory_limit = 0);

/// False when EDEN_NX2_OFFLINE_COMPUTE=0: compute rows are then counted as unsupported_stage
/// (the off switch for A/B products).
[[nodiscard]] bool ComputeEnabled();
} // namespace VideoCommon::OfflineShaders
