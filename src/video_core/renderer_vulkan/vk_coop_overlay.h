// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <optional>

#include "core/frontend/framebuffer_layout.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {
class Device;
class MemoryAllocator;
class Scheduler;

/// The co-op overlay (toasts, name tags) copied into the composited present frame: after the
/// composition pass, before the frame is flushed and presented. No pipeline, no depth.
class CoopOverlay {
public:
    CoopOverlay(MemoryAllocator& allocator, Scheduler& scheduler);
    ~CoopOverlay();
    /// `image` is in GENERAL layout (the composition's final layout) and stays there.
    void Draw(VkImage image, VkFormat format, u32 width, u32 height,
              const Layout::FramebufferLayout& layout);

private:
    struct Slot {
        vk::Buffer buffer;
        u64 tick{};
        bool used{};
        VkDeviceSize capacity{};
        u64 panel_cache_id{};
        VkFormat panel_format{};
    };
    static constexpr VkDeviceSize SlotSize = 4u << 20;
    MemoryAllocator& allocator;
    Scheduler& scheduler;
    std::array<Slot, 4> slots;
    std::optional<VkFormat> refused;
};
} // namespace Vulkan
