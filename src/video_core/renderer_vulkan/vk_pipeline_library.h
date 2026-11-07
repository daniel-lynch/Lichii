// SPDX-License-Identifier: GPL-3.0-or-later

// Graphics pipeline libraries (EDEN_NX2_PIPELINE_LIBRARY=1, docs/pipeline-library.md): the
// process-wide cache of library parts and the fast link. Off, nothing here is reached.

#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>

#include "common/common_types.h"
#include "video_core/renderer_vulkan/pipeline_library.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class DescriptorLayoutBuilder;
class Device;

/// Remembers the SPIR-V content hash of every shader module BuildShader makes while libraries
/// are enabled: a library is reused by module content, not by handle.
void RecordShaderModuleContent(const Device& device, VkShaderModule module,
                               std::span<const u32> code);
[[nodiscard]] std::optional<u128> ShaderModuleContent(VkShaderModule module);

class PipelineLibraryCache {
public:
    /// What one pipeline's link did (logged with its build).
    struct LinkResult {
        vk::Pipeline pipeline;           ///< Null: build it monolithic.
        u8 built_parts{};                ///< Parts this call compiled (bit per Part).
        u8 waited_parts{};               ///< Parts another thread was compiling.
        double library_ms{};             ///< Time in library creation and waits.
        double link_ms{};                ///< Time in the fast link.
    };

    struct Layout {
        VkPipelineLayout layout{};
        u128 identity{};
    };

    /// Whether graphics pipelines use libraries (switch on and the device supports them).
    [[nodiscard]] static bool Enabled(const Device& device_) noexcept;

    /// Whether task/mesh pipelines also do (EDEN_NX2_PIPELINE_LIBRARY_MESH=0 keeps them
    /// monolithic).
    [[nodiscard]] static bool MeshEnabled() noexcept;

    /// The cache of this device, shared by every graphics pipeline that uses libraries. It
    /// lives until the last of them is destroyed, so its libraries never outlive the device.
    [[nodiscard]] static std::shared_ptr<PipelineLibraryCache> Acquire(const Device& device_);

    explicit PipelineLibraryCache(const Device& device);
    ~PipelineLibraryCache();

    PipelineLibraryCache(const PipelineLibraryCache&) = delete;
    PipelineLibraryCache& operator=(const PipelineLibraryCache&) = delete;

    /// A pipeline layout the cache owns, identically defined to the one the builder makes with
    /// these flags; libraries are created with it so that they outlive their first pipeline.
    [[nodiscard]] Layout LayoutFor(const DescriptorLayoutBuilder& builder, bool use_push_descriptor,
                                   bool use_descriptor_buffer);

    /// Links the plan's parts, compiling the ones not in the cache. layout is the pipeline's
    /// own layout (identically defined to library_layout); pipeline_cache is the cache handle
    /// monolithic pipelines use (null on NVIDIA), read per call because it may be replaced.
    [[nodiscard]] LinkResult Link(PipelineLibrary::Plan& plan, VkPipelineLayout library_layout,
                                  VkPipelineLayout layout, VkPipelineCache pipeline_cache);

    /// Counts a pipeline built monolithic because the plan could not split it.
    void NoteUnsupported(PipelineLibrary::Unsupported reason);

private:
    struct Entry {
        enum class State : u8 { Building, Ready, Failed };
        std::mutex mutex;
        std::condition_variable condition;
        State state{State::Building};
        vk::Pipeline pipeline;
    };

    struct OwnedLayout {
        vk::DescriptorSetLayout set_layout;
        vk::PipelineLayout layout;
    };

    struct Stats {
        std::array<u64, PipelineLibrary::NumParts> built{};
        std::array<double, PipelineLibrary::NumParts> built_ms{};
        std::array<double, PipelineLibrary::NumParts> built_max_ms{};
        std::array<u64, PipelineLibrary::NumParts> reused{};
        std::array<u64, PipelineLibrary::NumParts> waited{};
        std::array<u64, PipelineLibrary::NumParts> failed{};
        u64 links{};
        double link_ms{};
        double link_max_ms{};
        u64 link_failed{};
        std::array<u64, static_cast<size_t>(PipelineLibrary::Unsupported::Count)> unsupported{};
    };

    /// The part's library, compiling it here if no thread has; VK_NULL_HANDLE if it failed.
    VkPipeline GetOrBuild(PipelineLibrary::Part part, const u128& key,
                          const VkGraphicsPipelineCreateInfo& ci, VkPipelineCache pipeline_cache,
                          bool& built, bool& waited);

    void Report(bool final);

    const Device& device;

    std::mutex map_mutex;
    std::array<std::unordered_map<u128, std::shared_ptr<Entry>, PipelineLibrary::KeyHash>,
               PipelineLibrary::NumParts>
        libraries;

    std::mutex layout_mutex;
    std::unordered_map<u128, OwnedLayout, PipelineLibrary::KeyHash> layouts;

    std::mutex stats_mutex;
    Stats stats;
    std::chrono::steady_clock::time_point last_report{std::chrono::steady_clock::now()};
    u32 failure_logs{};
};

} // namespace Vulkan
