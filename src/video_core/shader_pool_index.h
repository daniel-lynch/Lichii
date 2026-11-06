// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/common_types.h"
#include "common/shader_aot_scene.h"

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
///
/// With EDEN_NX2_AOT_MANIFEST=<manifest-<Scene>.json, or a directory of them> (static scene
/// manifests built from the game's files, kept outside the repository), the programs a scene's
/// materials use are offered first, main material pass (tier 1) to tier 3, and the other
/// siblings only when no manifest program is waiting.
///
/// A directory that holds manifest-_global.json selects manifests per scene: the global one is
/// always loaded, and manifest-<Scene>.json when the game enters that scene (Common::ShaderAotScene,
/// posted by the RomFS builder from the scene's file reads). A new scene replaces the previous
/// scene's rows; seeds already stored stay. When the scene cannot be known (no scene file was
/// hooked, EDEN_NX2_AOT_SCENES=0, or no scene read within EDEN_NX2_AOT_SCENE_WAIT seconds, default
/// 120), every manifest in the directory is loaded and their rows are united, as before.
class ShaderPoolIndex {
public:
    /// The index named by EDEN_NX2_AOT_INDEX, or null when unset or unreadable.
    [[nodiscard]] static std::unique_ptr<ShaderPoolIndex> Open();
    ~ShaderPoolIndex();

    /// A native graphics program at `address` is used for the first time.
    void OnFirstUse(Tegra::MemoryManager& gpu_memory, GPUVAddr address);

    /// Per-scene manifests: picks up a scene the game started to load (cheap, for every draw).
    void PollScene(Tegra::MemoryManager& gpu_memory) {
        if (scene_mode && (scene_load_pending || ++scene_polls % 256 == 0 ||
                           Common::ShaderAotScene::Generation() != scene_generation)) {
            UpdateScene(gpu_memory);
        }
    }

    /// Candidate priority: manifest tiers 1-3, then the siblings no manifest names.
    static constexpr size_t PriorityCount = 4;
    static constexpr u8 NoTier = PriorityCount;

    /// A verified program of a recognised pool, not used yet, to translate ahead of time.
    struct Candidate {
        GPUVAddr address;
        u8 stage; ///< Container stage slot (0 vertex, 4 pixel; others are not offered)
        u8 tier;  ///< Manifest tier 1-3, NoTier when no manifest names the program
        u32 constants_offset;
        u16 constants_size;
        u32 manifest_bank1; ///< Index of the manifest's bank-1 words, ~0 if none
    };
    /// Up to `count` candidates, highest priority first and oldest first within a priority,
    /// skipping programs used since they were found. `manifest_only` takes manifest programs
    /// only. Each must then be acknowledged (seeded, or never seedable) or returned.
    [[nodiscard]] std::vector<Candidate> TakeCandidates(size_t count, bool manifest_only = false);
    /// The candidate was seeded or can never be: a later scene does not offer it again.
    void Acknowledge(const Candidate& candidate);
    /// Candidates taken but not handled (no room now) go back to the front of their queues, in
    /// order.
    void ReturnCandidates(std::span<const Candidate> candidates);
    /// Candidates waiting (for tests and statistics).
    [[nodiscard]] size_t QueuedCandidates() const noexcept {
        return candidates_queued;
    }
    /// Queues a candidate as recognition does (for tests).
    void QueueForTesting(const Candidate& candidate);
    /// Whether a manifest program waits for translation (cheap, for every draw).
    [[nodiscard]] bool HasManifestCandidates() const noexcept {
        return manifest_queued != 0;
    }
    /// Compares a candidate's bank-1 section read from its pool (authoritative) with the
    /// manifest's words, for the statistics.
    void CheckBank1(const Candidate& candidate, std::span<const u8> constants);
    /// A seeded candidate left the GPU thread for a worker (statistics by tier).
    void CountSeeded(const Candidate& candidate) {
        ++seeded[(std::min<size_t>)(candidate.tier, NoTier) - 1];
    }
    /// GPU-thread time spent building candidate environments, for the statistics.
    void AddSeedTime(std::chrono::steady_clock::duration duration) {
        seed_time += duration;
    }

    struct Program {
        u32 offset;
        u32 size;
        u8 stage;
        u64 fingerprint;
        u32 constants_offset; ///< Bank-1 constant section from the program start, ~0 if none
        u16 constants_size;
        u8 tier{NoTier};          ///< Lowest manifest tier naming the program
        u32 manifest_bank1{~0U}; ///< Index into manifest_bank1_words
        u8 global_tier{NoTier};   ///< The same, from the manifests kept across scenes
        u32 global_bank1{~0U};
    };
    struct Container {
        std::vector<Program> programs;
    };

    /// Index parsing and the fingerprint, exposed for tests.
    [[nodiscard]] static bool Parse(std::span<const u8> data, std::vector<Container>& out);
    [[nodiscard]] static u64 Fingerprint(std::span<const u8> bytes);

    /// One row of a scene manifest (oot-aot-static-manifest-1): a stage program keyed like the
    /// index (fingerprint, pool offset, padded size, stage slot).
    struct ManifestRow {
        u64 fingerprint;
        u32 pool_offset;
        u32 size;
        u8 stage; ///< Container stage slot, as in the index
        u8 tier;
        u32 bank1_offset{~0U};
        std::vector<u32> bank1_words;
    };
    /// Manifest parsing, exposed for tests. Rows of tier 4 and of unknown stages are skipped.
    [[nodiscard]] static bool ParseManifest(std::string_view json, std::vector<ManifestRow>& out);
    /// Marks the indexed programs a manifest names (lowest tier wins); returns the rows matched.
    size_t ApplyManifest(std::span<const ManifestRow> rows);
    /// The tiers marked so far are kept across scenes (the global manifest).
    void KeepTiersAsGlobal();
    /// Replaces the previous scene's tiers with `rows` (global tiers stay); returns the rows
    /// matched. Queued candidates are not touched (UpdateScene re-queues).
    size_t SelectSceneRows(std::span<const ManifestRow> rows);
    /// An index over `containers` without a file or manifests, for tests.
    [[nodiscard]] static std::unique_ptr<ShaderPoolIndex> ForTesting(
        std::vector<Container> containers) {
        return std::unique_ptr<ShaderPoolIndex>(new ShaderPoolIndex(std::move(containers)));
    }
    [[nodiscard]] const std::vector<Container>& Containers() const noexcept {
        return containers;
    }

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
    /// Addresses of programs verified in a recognised pool and not used yet, with their tier.
    std::unordered_map<GPUVAddr, u8> predicted;
    /// Addresses of programs already used.
    std::unordered_set<GPUVAddr> used;
    /// Candidates waiting for translation by priority, bounded by MaxCandidatesQueued together.
    std::array<std::deque<Candidate>, PriorityCount> candidates_queues;
    size_t candidates_queued{};
    size_t manifest_queued{};
    static constexpr size_t MaxCandidatesQueued = 4096;
    u64 candidates_offered{};
    u64 candidates_dropped{};
    size_t candidates_queued_peak{};

    /// Scene manifests (EDEN_NX2_AOT_MANIFEST).
    struct LoadedManifests {
        size_t files{};
        std::vector<ManifestRow> rows;
        std::chrono::steady_clock::duration time{};
    };
    static LoadedManifests ReadManifests(std::string path);
    void StartManifestLoad();
    /// Applies the manifests once loaded; false while they are still being read.
    bool PollManifest();
    std::future<LoadedManifests> manifest_load;
    bool manifest_pending{};
    std::unordered_multimap<u64, Program*> programs_by_key; ///< (fingerprint, offset) key

    /// Per-scene manifests (a directory with manifest-_global.json).
    void UpdateScene(Tegra::MemoryManager& gpu_memory);
    void ApplyScene(Tegra::MemoryManager& gpu_memory, LoadedManifests loaded);
    bool scene_mode{};
    std::filesystem::path manifest_dir;
    u64 scene_generation{};   ///< Generation of the last scene hint taken
    u64 scene_polls{};
    std::string scene_name;   ///< Scene whose rows are applied ("" none, "*" the union)
    std::string scene_loading;
    bool scene_load_pending{};
    std::future<LoadedManifests> scene_load;
    std::shared_ptr<std::atomic<bool>> scene_load_done;
    std::chrono::steady_clock::time_point scene_deadline{};
    size_t global_bank1_count{};
    /// Pools recognised so far: (container, pool address), to offer a new scene's programs.
    std::vector<std::pair<u32, GPUVAddr>> recognised_pools;
    /// Addresses acknowledged (seeded, or never seedable), not offered again.
    std::unordered_set<GPUVAddr> taken;
    u64 scenes_applied{};
    u64 scenes_without_manifest{};
    std::vector<GPUVAddr> deferred_first_uses;
    std::vector<std::vector<u32>> manifest_bank1_words;
    size_t manifest_files{};
    bool offer_others{true}; ///< Offer siblings no manifest names (EDEN_NX2_AOT_OTHERS=1)
    size_t manifest_rows{};
    size_t manifest_rows_matched{};
    size_t manifest_bytes{};
    std::chrono::steady_clock::duration manifest_time{};
    std::array<u64, PriorityCount> offered_by_tier{};
    std::array<u64, PriorityCount> seeded{};
    /// First uses of verified programs by tier: all, and those still waiting in the queue.
    std::array<u64, PriorityCount> first_uses_by_tier{};
    std::array<u64, PriorityCount> first_uses_still_queued{};
    /// Addresses of queued candidates (to tell a first use that came before its translation).
    std::unordered_map<GPUVAddr, u8> queued_tier;
    u64 bank1_agreed{};
    u64 bank1_disagreed{};
    u64 bank1_unchecked{};

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
    std::chrono::steady_clock::duration seed_time{};
};

} // namespace VideoCommon
