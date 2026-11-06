// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <filesystem>
#include <memory>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/common_types.h"

namespace Tegra {
class MemoryManager;
}

namespace VideoCommon {

/// Recognises the game's shader code pools in GPU memory (use_shader_aot_translation with
/// EDEN_NX2_AOT_INDEX=<index file>, written by tools/oot/container/aot_index.py from the user's own
/// game files). When a native graphics program is used for the first time, its first bytes are
/// looked up in the index; if they identify a container, every other program of that container is
/// expected at the same offset from the pool and is checked byte for byte (by fingerprint). It
/// translates nothing: it measures how many programs could be known before their first draw.
class ShaderPoolIndex {
public:
    /// The index named by EDEN_NX2_AOT_INDEX, or null when unset or unreadable.
    [[nodiscard]] static std::unique_ptr<ShaderPoolIndex> Open();
    ~ShaderPoolIndex();

    /// A native graphics program at `address` is used for the first time.
    void OnFirstUse(Tegra::MemoryManager& gpu_memory, GPUVAddr address);

    struct Program {
        u32 offset;
        u32 size;
        u8 stage;
        u64 fingerprint;
    };
    struct Container {
        std::vector<Program> programs;
    };

    /// Index parsing and the fingerprint, exposed for tests.
    [[nodiscard]] static bool Parse(std::span<const u8> data, std::vector<Container>& out);
    [[nodiscard]] static u64 Fingerprint(std::span<const u8> bytes);

private:
    explicit ShaderPoolIndex(std::vector<Container> containers);
    void LogStatistics(const char* when) const;

    static constexpr size_t FingerprintBytes = 256;
    /// A program found in more containers than this does not identify one (shared effects).
    static constexpr size_t MaxCandidates = 16;

    std::vector<Container> containers;
    /// Fingerprint -> (container, program) of every indexed program.
    std::unordered_multimap<u64, std::pair<u32, u32>> by_fingerprint;
    /// Program sizes under FingerprintBytes (their fingerprints cover fewer bytes).
    std::vector<u32> short_sizes;
    /// (container, pool address) pairs already checked.
    std::unordered_set<u64> checked;
    /// Addresses of programs verified in a recognised pool and not used yet.
    std::unordered_set<GPUVAddr> predicted;
    /// Addresses of programs already used.
    std::unordered_set<GPUVAddr> used;

    u64 first_uses{};
    u64 first_uses_indexed{};
    u64 first_uses_predicted{};
    u64 ambiguous{};
    u64 containers_recognised{};
    u64 containers_rejected{};
    u64 siblings_verified{};
    u64 siblings_mismatched{};
    u64 siblings_unmapped{};
    std::chrono::steady_clock::duration time{};
};

} // namespace VideoCommon
