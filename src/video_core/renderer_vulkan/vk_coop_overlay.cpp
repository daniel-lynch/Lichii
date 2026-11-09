// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <vector>

#include "common/logging.h"
#include "video_core/coop_overlay.h"
#include "video_core/renderer_vulkan/present/util.h"
#include "video_core/renderer_vulkan/vk_coop_overlay.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

namespace Vulkan {
namespace {
enum class Packing { Rgba, Bgra, A2B10G10R10, None };

Packing PackingOf(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        return Packing::Rgba;
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
        return Packing::Bgra;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        return Packing::A2B10G10R10;
    default:
        return Packing::None;
    }
}

void Pack(Packing packing, const u8* rgba, u8* out) {
    switch (packing) {
    case Packing::Rgba:
        std::memcpy(out, rgba, 4);
        out[3] = 255;
        break;
    case Packing::Bgra:
        out[0] = rgba[2], out[1] = rgba[1], out[2] = rgba[0], out[3] = 255;
        break;
    case Packing::A2B10G10R10: {
        const u32 r = rgba[0] * 1023u / 255u, g = rgba[1] * 1023u / 255u, b = rgba[2] * 1023u / 255u;
        const u32 word = r | (g << 10) | (b << 20) | (3u << 30);
        std::memcpy(out, &word, 4);
        break;
    }
    case Packing::None:
        break;
    }
}

constexpr VkImageSubresourceRange Range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
constexpr VkImageSubresourceLayers Layers{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
} // namespace

CoopOverlay::CoopOverlay(MemoryAllocator& allocator_, Scheduler& scheduler_)
    : allocator{allocator_}, scheduler{scheduler_} {}

CoopOverlay::~CoopOverlay() {
    scheduler.Finish();
}

void CoopOverlay::Draw(VkImage image, VkFormat format, u32 width, u32 height,
                       const Layout::FramebufferLayout& layout) {
    const auto frame = Core::OotCoop::Ux::Current();
    if (frame.Empty()) return;
    const Packing packing = PackingOf(format);
    if (packing == Packing::None) {
        if (refused != format) {
            refused = format;
            LOG_WARNING(Render_Vulkan, "OoT co-op overlay: present format {} not supported; overlay off",
                        static_cast<int>(format));
        }
        return;
    }
    const VideoCore::CoopOverlay::Rect screen{
        static_cast<s32>(layout.screen.left), static_cast<s32>(layout.screen.top),
        static_cast<s32>(layout.screen.right), static_cast<s32>(layout.screen.bottom)};
    const auto boxes = VideoCore::CoopOverlay::Layout(frame, screen, width, height);
    if (boxes.empty()) return;
    Slot* slot = nullptr;
    for (auto& candidate : slots) {
        if (!candidate.used || scheduler.IsFree(candidate.tick)) {
            slot = &candidate;
            break;
        }
    }
    if (!slot) return; // every buffer still in flight: skip this frame's overlay
    if (!slot->buffer) slot->buffer = CreateWrappedBuffer(allocator, SlotSize, MemoryUsage::Upload);
    auto mapped = slot->buffer.Mapped();
    std::vector<VkBufferImageCopy> copies;
    VkDeviceSize offset = 0;
    for (const auto& box : boxes) {
        const VkDeviceSize bytes = VkDeviceSize{box.width} * box.height * 4;
        if (offset + bytes > SlotSize || mapped.size() < offset + bytes) break;
        u8* out = mapped.data() + offset;
        for (size_t i = 0; i < size_t{box.width} * box.height; ++i) {
            Pack(packing, &box.rgba[i * 4], out + i * 4);
        }
        copies.push_back({.bufferOffset = offset,
                          .bufferRowLength = 0,
                          .bufferImageHeight = 0,
                          .imageSubresource = Layers,
                          .imageOffset = {box.x, box.y, 0},
                          .imageExtent = {box.width, box.height, 1}});
        offset = (offset + bytes + 15) & ~VkDeviceSize{15};
    }
    if (copies.empty()) return;
    slot->buffer.Flush();
    slot->used = true;
    slot->tick = scheduler.CurrentTick();
    const VkBuffer buffer = *slot->buffer;
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([image, buffer, copies = std::move(copies)](vk::CommandBuffer cmdbuf) {
        const VkImageMemoryBarrier before{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = Range};
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                               before);
        cmdbuf.CopyBufferToImage(buffer, image, VK_IMAGE_LAYOUT_GENERAL, copies);
        VkImageMemoryBarrier after = before;
        after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        after.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                               after);
    });
}
} // namespace Vulkan
