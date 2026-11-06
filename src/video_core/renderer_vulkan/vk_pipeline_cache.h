// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include "common/container/unordered_map.h"
#include <vector>

#include "common/common_types.h"
#include "common/thread_worker.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/value.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/renderer_vulkan/fixed_pipeline_state.h"
#include "video_core/renderer_vulkan/vk_buffer_cache.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache_prebuild.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/shader_cache.h"
#include "video_core/shader_translation_cache.h"

namespace Core {
class System;
}

namespace Shader::IR {
struct Program;
}

namespace VideoCore {
class ShaderNotify;
}

namespace VideoCommon {
class ReplayEnvironment;
}

namespace Vulkan {

using Maxwell = Tegra::Engines::Maxwell3D::Regs;

struct ComputePipelineCacheKey {
    u64 unique_hash;
    u32 shared_memory_size;
    std::array<u32, 3> workgroup_size;

    size_t Hash() const noexcept;

    bool operator==(const ComputePipelineCacheKey& rhs) const noexcept;

    bool operator!=(const ComputePipelineCacheKey& rhs) const noexcept {
        return !operator==(rhs);
    }
};
static_assert(std::has_unique_object_representations_v<ComputePipelineCacheKey>);
static_assert(std::is_trivially_copyable_v<ComputePipelineCacheKey>);
static_assert(std::is_trivially_constructible_v<ComputePipelineCacheKey>);

} // namespace Vulkan

namespace std {

template <>
struct hash<Vulkan::ComputePipelineCacheKey> {
    size_t operator()(const Vulkan::ComputePipelineCacheKey& k) const noexcept {
        return k.Hash();
    }
};

} // namespace std

namespace Vulkan {

class ComputePipeline;
class DescriptorPool;
class Device;
class PipelineStatistics;
class RenderPassCache;
class Scheduler;

using VideoCommon::ShaderInfo;

struct ShaderPools {
    void ReleaseContents() {
        flow_block.ReleaseContents();
        block.ReleaseContents();
        inst.ReleaseContents();
    }

    Shader::ObjectPool<Shader::IR::Inst> inst{8192};
    Shader::ObjectPool<Shader::IR::Block> block{32};
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_block{32};
};

struct ComputePipelineCacheEntry {
    std::unique_ptr<ComputePipeline> pipeline;
    std::string failure;

    [[nodiscard]] ComputePipeline* Get(bool native, GPUVAddr address,
                                       const ComputePipelineCacheKey& key) const;
};

class PipelineCache : public VideoCommon::ShaderCache {
public:
    explicit PipelineCache(Tegra::MaxwellDeviceMemoryManager& device_memory_, const Device& device,
                           Scheduler& scheduler, DescriptorPool& descriptor_pool,
                           GuestDescriptorQueue& guest_descriptor_queue,
                           DescriptorBufferRing& descriptor_buffer_ring,
                           RenderPassCache& render_pass_cache, BufferCache& buffer_cache,
                           TextureCache& texture_cache, VideoCore::ShaderNotify& shader_notify_);
    ~PipelineCache();

    [[nodiscard]] GraphicsPipeline* CurrentGraphicsPipeline();

    void TickFrame();

    [[nodiscard]] ComputePipeline* CurrentComputePipeline();

    void LoadDiskResources(u64 title_id, std::stop_token stop_loading,
                           const VideoCore::DiskResourceLoadCallback& callback);

private:
    [[nodiscard]] GraphicsPipeline* CurrentGraphicsPipelineSlowPath();

    [[nodiscard]] GraphicsPipeline* BuiltPipeline(GraphicsPipeline* pipeline) const noexcept;

    std::unique_ptr<GraphicsPipeline> CreateGraphicsPipeline();

    std::unique_ptr<GraphicsPipeline> CreateGraphicsPipeline(
        ShaderPools& pools, const GraphicsPipelineCacheKey& key,
        std::span<Shader::Environment* const> envs, PipelineStatistics* statistics,
        bool build_in_parallel, std::shared_ptr<ShaderPools> emit_pools = nullptr,
        std::unique_ptr<EmissionBudget::Reservation> reservation = nullptr);

    ComputePipelineCacheEntry CreateComputePipeline(const ComputePipelineCacheKey& key,
                                                     const ShaderInfo* shader);

    ComputePipelineCacheEntry CreateComputePipeline(ShaderPools& pools,
                                                     const ComputePipelineCacheKey& key,
                                                     Shader::Environment& env,
                                                     PipelineStatistics* statistics,
                                                     bool build_in_parallel);

    void SerializeVulkanPipelineCache(const std::filesystem::path& filename,
                                      const vk::PipelineCache& pipeline_cache, u32 cache_version);

    vk::PipelineCache LoadVulkanPipelineCache(const std::filesystem::path& filename,
                                              u32 expected_cache_version);

    void QueueVulkanPipelineCacheFlush();

    const Device& device;
    Scheduler& scheduler;
    DescriptorPool& descriptor_pool;
    GuestDescriptorQueue& guest_descriptor_queue;
    DescriptorBufferRing& descriptor_buffer_ring;
    RenderPassCache& render_pass_cache;
    BufferCache& buffer_cache;
    TextureCache& texture_cache;
    VideoCore::ShaderNotify& shader_notify;
    bool use_asynchronous_shaders{};
    bool use_vulkan_pipeline_cache{};
    const size_t num_pipeline_workers;
    EmissionBudget emission_budget;

    GraphicsPipelineCacheKey graphics_key{};
    GraphicsPipeline* current_pipeline{};

    /// Destroys graphics pipelines unused for a while, once the GPU is done with them.
    void EvictUnusedPipelines();
    u64 frame_count{};
    std::deque<std::pair<u64, std::unique_ptr<GraphicsPipeline>>> evicted_pipelines; ///< (tick, pipeline)
    std::unordered_set<u64> used_and_evicted; ///< Key hashes of pipelines used this session, evicted since

    ::Common::unordered_map<ComputePipelineCacheKey, ComputePipelineCacheEntry> compute_cache;
    ::Common::unordered_map<GraphicsPipelineCacheKey, std::unique_ptr<GraphicsPipeline>> graphics_cache;

    ShaderPools main_pools;
    std::mutex spare_pools_mutex;
    std::vector<std::shared_ptr<ShaderPools>> spare_pools;

    Shader::Profile profile;
    Shader::HostTranslateInfo host_info;

    /// Translated shader stages (use_shader_translation_cache), kept across pipeline evictions.
    VideoCommon::TranslationCache::Store translation_cache;
    VideoCommon::TranslationCache::Digest translation_config{};
    bool translation_cache_verify{};
    std::atomic<u64> translation_cache_verified{};
    std::atomic<u64> translation_cache_mismatches{};
    std::chrono::steady_clock::time_point translation_cache_logged{};
    void OpenTranslationCache(const std::filesystem::path& filename);
    [[nodiscard]] bool IsTranslationCacheable(const Shader::Environment& env) const;

    /// use_shader_aot_translation (or EDEN_NX2_AOT_TRANSLATE=1): task and mesh stages are cached
    /// too, and each run-time graphics stage is counted by why it was or was not taken from the
    /// cache.
    bool aot_translation{};
    enum class StageOutcome : u8 {
        Hit,
        Uncacheable,
        NewKey,
        ReplayFailed,
        VariantMiss,
        VariantDeferred,
        Count
    };
    std::atomic<u64> replay_translations{};
    std::atomic<u64> replay_translation_ns{};
    std::atomic<u64> replay_translation_mismatches{};
    struct OutcomeCounter {
        std::atomic<u64> stages{};
        std::atomic<u64> ns{};
    };
    /// [outcome][0 = task/mesh, 1 = other stages]
    std::array<std::array<OutcomeCounter, 2>, static_cast<size_t>(StageOutcome::Count)>
        stage_outcomes{};
    std::atomic<u64> boot_pipelines_failed{};
    std::atomic<u64> boot_mesh_pipelines{};
    void CountStageOutcome(StageOutcome outcome, Shader::Stage stage, u64 ns);
    void LogStageOutcomes(std::string_view when);

    /// Ahead-of-time translation of recognised pool programs (ShaderCache::seed_stage): the
    /// GPU thread builds the environment and an owned copy of it; a worker translates the copy,
    /// which answers no queries, and stores programs that needed none as ordinary entries.
    bool SeedStage(VideoCommon::GenericEnvironment& env, u64 unique_hash, size_t slot,
                   std::span<const u8> constants, u8 tier);
    [[nodiscard]] static size_t AotWorkerCount();
    size_t aot_worker_count{};
    static constexpr u64 MaxAotPending = 32;
    static constexpr u64 MaxAotEntries = 4096;
    std::atomic<u64> aot_pending{};
    std::atomic<u64> aot_reserved{}; ///< Admitted jobs plus stored entries, against MaxAotEntries
    [[nodiscard]] bool AotHasRoom() const noexcept {
        return aot_translation && aot_pending.load() < MaxAotPending &&
               aot_reserved.load() < MaxAotEntries;
    }
    std::atomic<u64> aot_candidates{};
    std::atomic<u64> aot_skipped_known{};
    std::atomic<u64> aot_skipped_uncacheable{};
    std::atomic<u64> aot_skipped_full{};
    std::atomic<u64> aot_completed{};
    std::atomic<u64> aot_failed{};
    std::array<std::atomic<u64>, 16> aot_needs_query{}; ///< By TranslationCache::QueryKind
    std::atomic<u64> aot_capture_ns{};
    std::atomic<u64> aot_translate_ns{};
    std::atomic<u64> aot_entry_hits{};
    std::atomic<u64> aot_with_constants{};
    std::mutex aot_mutex;
    /// Stage keys of entries the workers stored, not yet used by a draw, with their tier index.
    std::unordered_map<VideoCommon::TranslationCache::Digest, u8,
                       VideoCommon::TranslationCache::DigestHash>
        aot_keys;
    /// Stage keys of jobs queued or running.
    std::unordered_set<VideoCommon::TranslationCache::Digest, VideoCommon::TranslationCache::DigestHash>
        aot_in_flight;
    std::array<std::atomic<u64>, 4> aot_stored_by_tier{}; ///< Tiers 1-3, then no manifest tier
    std::array<std::atomic<u64>, 4> aot_hits_by_tier{};
    std::atomic<u64> aot_late{};
    std::atomic<u64> aot_variant_meta_differs{};
    std::array<std::atomic<u64>, GraphicsPipeline::AotClasses> pipelines_by_class{};
    /// (unique hash, slot) of admitted seeds -> their stage key, until the program's first draw.
    std::unordered_map<u64, VideoCommon::TranslationCache::Digest> aot_seeded_programs;
    std::atomic<u64> aot_miss_key_differs{};
    std::atomic<u64> aot_miss_replay_failed{};
    std::atomic<u64> aot_miss_not_stored{};

    /// Seeded variants: a seeded entry also gets the SPIR-V module of the emission recipe
    /// (runtime info and bindings in) most often seen at run time for stages of its class (stage
    /// and loaded varyings), so its first draw neither translates nor emits. A wrong guess is
    /// never used: the variant key holds the runtime info and the bindings. Off with
    /// EDEN_NX2_AOT_EMIT=0.
    struct EmissionRecipe {
        Shader::RuntimeInfo runtime_info;
        Shader::Backend::Bindings binding_in{};
        VideoCommon::TranslationCache::Digest digest{};
        u64 count{};
        bool seen{}; ///< Recorded this session (false: only from the recipe file)
    };
    bool aot_emit_variants{};
    std::mutex recipe_mutex;
    /// Class key -> recipes seen (a few per class; key 0 of a stage holds the stage's recipes).
    std::unordered_map<u64, std::vector<EmissionRecipe>> recipes;
    [[nodiscard]] static u64 RecipeClass(Shader::Stage stage, const Shader::Info* info);
    [[nodiscard]] static u64 RecipeClassOf(Shader::Stage stage, const std::bitset<512>* loads);
    /// What each class key stands for (stage, loaded varyings or none), to write it out.
    struct RecipeClassId {
        Shader::Stage stage;
        bool by_loads;
        std::bitset<512> loads;
    };
    std::unordered_map<u64, RecipeClassId> recipe_class_ids; ///< Under recipe_mutex
    /// The recipe histogram is kept next to the translation file
    /// (vulkan_translations.<tag>.recipes.fork-vN.bin), so a later session guesses from the
    /// start. Loaded when the translation file is opened, written every 30 s when it changed
    /// and at shutdown.
    std::filesystem::path recipes_filename;
    bool recipes_dirty{}; ///< Under recipe_mutex
    size_t recipes_loaded{};
    size_t recipe_classes_loaded{};
    void LoadRecipes();
    void SaveRecipes();
    std::atomic<u64> aot_variants_from_file{}; ///< Seeds emitted with a recipe not seen yet
    /// A plain vertex-B + fragment pipeline's stage was emitted (or taken from the cache) with
    /// this runtime info and these bindings in.
    void RecordRecipe(Shader::Stage stage, const Shader::Info& info,
                      const Shader::RuntimeInfo& runtime_info,
                      const Shader::Backend::Bindings& binding_in);
    /// The most common recipe of the program's class, else of its stage; `class_match` says
    /// which.
    [[nodiscard]] std::optional<EmissionRecipe> BestRecipe(Shader::Stage stage,
                                                           const Shader::Info& info,
                                                           bool& class_match);
    std::atomic<u64> aot_variants_class{};      ///< Emitted with the class's recipe
    std::atomic<u64> aot_variants_stage{};      ///< Emitted with the stage's recipe
    std::atomic<u64> aot_variants_none{};       ///< No recipe seen yet for the stage
    std::atomic<u64> aot_variants_failed{};
    std::atomic<u64> aot_variant_ns{};
    std::atomic<u64> aot_full_hits{};           ///< Seeded stages whose first draw found the module
    std::atomic<u64> aot_emitted_at_draw{};     ///< Seeded stages emitted at their first draw
    std::atomic<u64> worker_variant_hits{};     ///< Deferred stages whose module the worker found
    std::atomic<u64> worker_variant_info_differs{};
    /// Seeds emitted with a stage-level guess wait (owning their environment) for their
    /// class's first recipe, then get a module for it on an AOT worker.
    struct WaitingSeed {
        std::shared_ptr<VideoCommon::ReplayEnvironment> env;
        std::shared_ptr<const VideoCommon::TranslationCache::Entry> entry;
        u64 bytes{};
        u64 generation{};
    };
    std::unordered_map<u64, std::vector<WaitingSeed>> waiting_seeds; ///< Under recipe_mutex
    size_t waiting_count{};
    void ParkSeed(const std::shared_ptr<VideoCommon::ReplayEnvironment>& env,
                  const std::shared_ptr<const VideoCommon::TranslationCache::Entry>& entry,
                  Shader::Stage stage, const Shader::Info& info, u64 bytes, u64 generation);
    void EmitWaitingSeeds(std::vector<WaitingSeed> seeds);
    /// Seeds whose class's recipe was seen, waiting for a pending slot (under recipe_mutex).
    std::deque<WaitingSeed> ready_seeds;
    size_t ready_seeds_peak{};
    void PumpReadySeeds();
    void EmitWaitingSeed(const WaitingSeed& seed);
    std::atomic<u64> aot_parked{};
    std::atomic<u64> aot_waiting_dropped{};
    std::atomic<u64> aot_late_variants{};
    std::atomic<u64> aot_late_info_differs{};
    std::atomic<u64> canonical_verified{};
    std::atomic<u64> canonical_mismatches{};
    std::atomic<u64> aot_bytes{};
    std::atomic<u64> aot_bytes_peak{};

    std::filesystem::path pipeline_cache_filename;

    /// Key hashes of the graphics pipelines in the disk cache. Only those a recent session used
    /// (used_pipelines) are built at boot; the rest are built when first needed, and not stored
    /// again. Each built pipeline holds driver memory (title 1: 6.5k cached, a quarter used).
    std::unordered_set<u64> disk_pipelines;
    std::unordered_map<u64, u8> used_pipelines; ///< Key hash -> sessions since it was last used
    std::unordered_set<const GraphicsPipeline*> used_this_session;
    std::filesystem::path used_pipelines_filename;
    std::chrono::steady_clock::time_point used_pipelines_saved{};
    void SaveUsedPipelines();

    std::filesystem::path vulkan_pipeline_cache_filename;
    vk::PipelineCache vulkan_pipeline_cache;
    size_t pipelines_since_flush{};
    std::chrono::steady_clock::time_point last_flush{};
    std::atomic<size_t> last_cache_size{};
    std::atomic_bool flush_in_flight{};

    Common::ThreadWorker workers;
    Common::ThreadWorker serialization_thread;
    /// Created on the first AOT job, so the setting off starts no thread. Declared after the
    /// translation cache: destroyed (and joined) before it.
    std::unique_ptr<Common::ThreadWorker> aot_workers;
    DynamicFeatures dynamic_features;
    PipelinePrebuild<GraphicsPipelineCacheKey, GraphicsPipeline> background_pipelines;
};

} // namespace Vulkan
