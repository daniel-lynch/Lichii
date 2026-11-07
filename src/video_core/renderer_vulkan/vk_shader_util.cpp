// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <cstring>
#include <vector>

#include "common/common_types.h"
#include "common/logging.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/spirv_inline_control.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

vk::ShaderModule BuildShader(const Device& device, std::span<const u32> code) {
    // EDEN_NX2_GRAPHICS_NOINLINE: graphics modules build the functions they call from several
    // places as calls, as compute modules do (EDEN_NX2_COMPUTE_NOINLINE, applied before this).
    // Only the module changes: the translation cache keeps the SPIR-V as emitted.
    std::vector<u32> storage;
    std::size_t marked = 0;
    code = VideoCommon::GraphicsModuleCode(code, VideoCommon::GraphicsNoInlineStages(), storage,
                                           marked);
    if (marked != 0) {
        static std::atomic<u32> logged{};
        if (logged.fetch_add(1, std::memory_order_relaxed) < 32) {
            LOG_INFO(Render_Vulkan, "Graphics module (stage {}): {} shared functions built as calls, "
                     "not inlined",
                     static_cast<u32>(VideoCommon::ModuleStage(code)), marked);
        }
    }
    return device.GetLogical().CreateShaderModule({
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .codeSize = static_cast<u32>(code.size_bytes()),
        .pCode = code.data(),
    });
}

} // namespace Vulkan
