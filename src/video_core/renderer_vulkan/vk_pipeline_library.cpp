// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstdlib>
#include <string>

#include <fmt/format.h>

#include "common/logging.h"
#include "video_core/renderer_vulkan/pipeline_helper.h"
#include "video_core/renderer_vulkan/vk_pipeline_library.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {
namespace {
using PipelineLibrary::KeyWriter;
using PipelineLibrary::NumParts;
using PipelineLibrary::Part;
using PipelineLibrary::Unsupported;

constexpr auto ReportInterval = std::chrono::seconds{10};
constexpr u32 MaxFailureLogs = 20;

double Milliseconds(std::chrono::steady_clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

struct ModuleRegistry {
    std::mutex mutex;
    // A handle is recorded again when the driver reuses it for a new module, so a live
    // module's entry is always its own. Entries of destroyed modules stay (16 bytes each).
    std::unordered_map<VkShaderModule, u128> content;
};

ModuleRegistry& Modules() {
    static ModuleRegistry registry;
    return registry;
}

std::mutex& AcquireMutex() {
    static std::mutex mutex;
    return mutex;
}

std::weak_ptr<PipelineLibraryCache>& Current() {
    static std::weak_ptr<PipelineLibraryCache> current;
    return current;
}

const Device*& CurrentDevice() {
    static const Device* device{};
    return device;
}

std::string PartList(u8 parts) {
    std::string text;
    for (size_t index = 0; index < NumParts; ++index) {
        if ((parts >> index) & 1) {
            text += text.empty() ? "" : "+";
            text += PipelineLibrary::PartName(static_cast<Part>(index));
        }
    }
    return text.empty() ? "none" : text;
}
} // Anonymous namespace

void RecordShaderModuleContent(const Device& device, VkShaderModule module,
                               std::span<const u32> code) {
    if (!PipelineLibraryCache::Enabled(device) || module == VK_NULL_HANDLE) {
        return;
    }
    const u128 hash = Common::CityHash128(reinterpret_cast<const char*>(code.data()),
                                          code.size_bytes());
    ModuleRegistry& registry = Modules();
    std::scoped_lock lock{registry.mutex};
    registry.content.insert_or_assign(module, hash);
}

std::optional<u128> ShaderModuleContent(VkShaderModule module) {
    ModuleRegistry& registry = Modules();
    std::scoped_lock lock{registry.mutex};
    const auto it = registry.content.find(module);
    if (it == registry.content.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool PipelineLibraryCache::Enabled(const Device& device_) noexcept {
    // The device enables the extension only when EDEN_NX2_PIPELINE_LIBRARY=1 asked for it.
    return device_.IsExtGraphicsPipelineLibrarySupported();
}

bool PipelineLibraryCache::MeshEnabled() noexcept {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_PIPELINE_LIBRARY_MESH");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

std::shared_ptr<PipelineLibraryCache> PipelineLibraryCache::Acquire(const Device& device_) {
    std::scoped_lock lock{AcquireMutex()};
    if (auto current = Current().lock(); current && CurrentDevice() == &device_) {
        return current;
    }
    auto created = std::make_shared<PipelineLibraryCache>(device_);
    Current() = created;
    CurrentDevice() = &device_;
    return created;
}

PipelineLibraryCache::PipelineLibraryCache(const Device& device_) : device{device_} {
    LOG_INFO(Render_Vulkan,
             "Nx2Perf pipeline_library: graphics pipelines are linked from libraries "
             "(mesh={}, link-time optimization off)",
             MeshEnabled());
}

PipelineLibraryCache::~PipelineLibraryCache() {
    Report(true);
}

PipelineLibraryCache::Layout PipelineLibraryCache::LayoutFor(
    const DescriptorLayoutBuilder& builder, bool use_push_descriptor, bool use_descriptor_buffer) {
    // Everything that makes two pipeline layouts identically defined: the set layout's flags
    // and bindings (with their binding flags) and the push constant range.
    KeyWriter writer{0x4C41594Fu}; // "LAYO"
    writer.Put(static_cast<u32>(use_push_descriptor));
    writer.Put(static_cast<u32>(use_descriptor_buffer));
    writer.Put(static_cast<u32>(builder.UsesPartiallyBound(use_push_descriptor)));
    const auto bindings = builder.Bindings();
    writer.Put(static_cast<u32>(bindings.size()));
    for (const VkDescriptorSetLayoutBinding& binding : bindings) {
        writer.Put(binding.binding);
        writer.Put(binding.descriptorType);
        writer.Put(binding.descriptorCount);
        writer.Put(binding.stageFlags);
    }
    const VkPushConstantRange range = builder.PushConstantRange();
    writer.Put(range.stageFlags);
    writer.Put(range.offset);
    writer.Put(range.size);
    const u128 identity = writer.Finish();

    std::scoped_lock lock{layout_mutex};
    const auto it = layouts.find(identity);
    if (it != layouts.end()) {
        return Layout{*it->second.layout, identity};
    }
    OwnedLayout owned;
    owned.set_layout = builder.CreateDescriptorSetLayout(use_push_descriptor, use_descriptor_buffer);
    owned.layout = builder.CreatePipelineLayout(*owned.set_layout);
    const VkPipelineLayout handle = *owned.layout;
    layouts.emplace(identity, std::move(owned));
    return Layout{handle, identity};
}

VkPipeline PipelineLibraryCache::GetOrBuild(Part part, const u128& key,
                                            const VkGraphicsPipelineCreateInfo& ci,
                                            VkPipelineCache pipeline_cache, bool& built,
                                            bool& waited) {
    std::shared_ptr<Entry> entry;
    bool builder = false;
    {
        std::scoped_lock lock{map_mutex};
        auto [it, inserted] = libraries[static_cast<size_t>(part)].try_emplace(key);
        if (inserted) {
            it->second = std::make_shared<Entry>();
            builder = true;
        }
        entry = it->second;
    }
    if (builder) {
        built = true;
        vk::Pipeline pipeline;
        bool failed = false;
        try {
            pipeline = device.GetLogical().CreateGraphicsPipeline(ci, pipeline_cache);
        } catch (const vk::Exception& exception) {
            failed = true;
            std::scoped_lock lock{stats_mutex};
            if (failure_logs < MaxFailureLogs) {
                ++failure_logs;
                LOG_WARNING(Render_Vulkan,
                            "Nx2Perf pipeline_library: {} library failed ({}), its pipelines are "
                            "built monolithic",
                            PipelineLibrary::PartName(part), exception.what());
            }
        }
        std::scoped_lock lock{entry->mutex};
        entry->pipeline = std::move(pipeline);
        entry->state = failed || !entry->pipeline ? Entry::State::Failed : Entry::State::Ready;
        entry->condition.notify_all();
        return entry->state == Entry::State::Ready ? *entry->pipeline : VK_NULL_HANDLE;
    }
    std::unique_lock lock{entry->mutex};
    if (entry->state == Entry::State::Building) {
        waited = true;
        entry->condition.wait(lock, [&] { return entry->state != Entry::State::Building; });
    }
    return entry->state == Entry::State::Ready ? *entry->pipeline : VK_NULL_HANDLE;
}

PipelineLibraryCache::LinkResult PipelineLibraryCache::Link(PipelineLibrary::Plan& plan,
                                                            VkPipelineLayout library_layout,
                                                            VkPipelineLayout layout,
                                                            VkPipelineCache pipeline_cache) {
    LinkResult result;
    std::array<VkPipeline, NumParts> parts{};
    std::array<double, NumParts> part_ms{};
    u32 num_parts = 0;
    u8 failed_parts = 0;
    for (size_t index = 0; index < NumParts; ++index) {
        const Part part = static_cast<Part>(index);
        if (!plan.HasPart(part)) {
            continue;
        }
        const auto begin = std::chrono::steady_clock::now();
        const VkGraphicsPipelineCreateInfo ci = plan.LibraryInfo(part, library_layout);
        bool built = false;
        bool waited = false;
        const VkPipeline library = GetOrBuild(part, plan.Key(part), ci, pipeline_cache, built, waited);
        part_ms[index] = Milliseconds(std::chrono::steady_clock::now() - begin);
        result.library_ms += part_ms[index];
        result.built_parts |= static_cast<u8>(built ? 1U << index : 0U);
        result.waited_parts |= static_cast<u8>(waited ? 1U << index : 0U);
        if (library == VK_NULL_HANDLE) {
            failed_parts |= static_cast<u8>(1U << index);
            continue;
        }
        parts[num_parts++] = library;
    }

    bool link_failed = false;
    if (failed_parts == 0) {
        const VkPipelineLibraryCreateInfoKHR library_ci{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR,
            .pNext = nullptr,
            .libraryCount = num_parts,
            .pLibraries = parts.data(),
        };
        // No VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT: the fast link is the point.
        const VkGraphicsPipelineCreateInfo link_ci{
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .pNext = &library_ci,
            .flags = plan.Flags(),
            .stageCount = 0,
            .pStages = nullptr,
            .pVertexInputState = nullptr,
            .pInputAssemblyState = nullptr,
            .pTessellationState = nullptr,
            .pViewportState = nullptr,
            .pRasterizationState = nullptr,
            .pMultisampleState = nullptr,
            .pDepthStencilState = nullptr,
            .pColorBlendState = nullptr,
            .pDynamicState = nullptr,
            .layout = layout,
            .renderPass = plan.RenderPass(),
            .subpass = plan.Subpass(),
            .basePipelineHandle = VK_NULL_HANDLE,
            .basePipelineIndex = 0,
        };
        const auto begin = std::chrono::steady_clock::now();
        try {
            result.pipeline = device.GetLogical().CreateGraphicsPipeline(link_ci, pipeline_cache);
        } catch (const vk::Exception& exception) {
            link_failed = true;
            std::scoped_lock lock{stats_mutex};
            if (failure_logs < MaxFailureLogs) {
                ++failure_logs;
                LOG_WARNING(Render_Vulkan,
                            "Nx2Perf pipeline_library: link failed ({}), pipeline built "
                            "monolithic",
                            exception.what());
            }
        }
        result.link_ms = Milliseconds(std::chrono::steady_clock::now() - begin);
        link_failed |= !result.pipeline;
    }

    bool report = false;
    {
        std::scoped_lock lock{stats_mutex};
        for (size_t index = 0; index < NumParts; ++index) {
            if (!plan.HasPart(static_cast<Part>(index))) {
                continue;
            }
            const u8 bit = static_cast<u8>(1U << index);
            if ((failed_parts & bit) != 0) {
                ++stats.failed[index];
            } else if ((result.built_parts & bit) != 0) {
                ++stats.built[index];
                stats.built_ms[index] += part_ms[index];
                stats.built_max_ms[index] = std::max(stats.built_max_ms[index], part_ms[index]);
            } else if ((result.waited_parts & bit) != 0) {
                ++stats.waited[index];
            } else {
                ++stats.reused[index];
            }
        }
        if (failed_parts != 0 || link_failed) {
            ++stats.link_failed;
        } else {
            ++stats.links;
            stats.link_ms += result.link_ms;
            stats.link_max_ms = std::max(stats.link_max_ms, result.link_ms);
        }
        report = std::chrono::steady_clock::now() - last_report >= ReportInterval;
    }
    if (failed_parts != 0) {
        LOG_INFO(Render_Vulkan, "Nx2Perf pipeline_library: fallback to monolithic, failed {}",
                 PartList(failed_parts));
    }
    if (report) {
        Report(false);
    }
    return result;
}

void PipelineLibraryCache::NoteUnsupported(Unsupported reason) {
    const size_t index = static_cast<size_t>(reason);
    bool first = false;
    {
        std::scoped_lock lock{stats_mutex};
        if (index < stats.unsupported.size()) {
            first = stats.unsupported[index]++ == 0;
        }
    }
    if (first) {
        LOG_INFO(Render_Vulkan,
                 "Nx2Perf pipeline_library: monolithic pipelines for {} (first of its kind)",
                 PipelineLibrary::UnsupportedName(reason));
    }
}

void PipelineLibraryCache::Report(bool final) {
    Stats copy;
    size_t resident = 0;
    {
        std::scoped_lock lock{stats_mutex};
        last_report = std::chrono::steady_clock::now();
        copy = stats;
    }
    {
        std::scoped_lock lock{map_mutex};
        for (const auto& map : libraries) {
            resident += map.size();
        }
    }
    std::string parts;
    for (size_t index = 0; index < NumParts; ++index) {
        parts += fmt::format(" {}=built:{}/{:.1f}ms/max:{:.1f}ms,reused:{},waited:{},failed:{}",
                             PipelineLibrary::PartName(static_cast<Part>(index)),
                             copy.built[index], copy.built_ms[index], copy.built_max_ms[index],
                             copy.reused[index], copy.waited[index], copy.failed[index]);
    }
    std::string unsupported;
    for (size_t index = 1; index < copy.unsupported.size(); ++index) {
        if (copy.unsupported[index] != 0) {
            unsupported += fmt::format(" {}:{}",
                                       PipelineLibrary::UnsupportedName(
                                           static_cast<Unsupported>(index)),
                                       copy.unsupported[index]);
        }
    }
    LOG_INFO(Render_Vulkan,
             "Nx2Perf pipeline_library{}: linked={} link_ms={:.3f} link_max_ms={:.3f} "
             "fallback={} monolithic:{}{} resident={}",
             final ? " (final)" : "", copy.links, copy.link_ms, copy.link_max_ms,
             copy.link_failed, unsupported.empty() ? " none" : unsupported, parts, resident);
}

} // namespace Vulkan
