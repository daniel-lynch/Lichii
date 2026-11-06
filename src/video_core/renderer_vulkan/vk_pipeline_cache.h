// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
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
#include "shader_recompiler/profile.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/renderer_vulkan/fixed_pipeline_state.h"
#include "video_core/renderer_vulkan/vk_buffer_cache.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
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
    void SeedStage(VideoCommon::GenericEnvironment& env, u64 unique_hash, size_t slot);
    static constexpr u64 MaxAotPending = 32;
    static constexpr u64 MaxAotEntries = 4096;
    std::atomic<u64> aot_pending{};
    std::atomic<u64> aot_reserved{}; ///< Admitted jobs plus stored entries, against MaxAotEntries
    std::atomic<u64> aot_generation{}; ///< Bumped when the translation cache is reopened
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
    std::mutex aot_mutex;
    std::unordered_set<VideoCommon::TranslationCache::Digest, VideoCommon::TranslationCache::DigestHash>
        aot_keys; ///< Stage keys of entries the workers stored, not yet used by a draw

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
};

} // namespace Vulkan
