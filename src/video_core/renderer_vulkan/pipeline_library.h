// SPDX-License-Identifier: GPL-3.0-or-later

// VK_EXT_graphics_pipeline_library: how one monolithic graphics pipeline create info splits into
// the four library parts, and the keys under which those parts are reused (EDEN_NX2_PIPELINE_
// LIBRARY=1, docs/pipeline-library.md). Pure: no device, so the split is unit tested.

#pragma once

#include <array>
#include <bit>
#include <cstring>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "common/cityhash.h"
#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan.h"

namespace Vulkan::PipelineLibrary {

enum class Part : u8 {
    VertexInput,      ///< Vertex bindings/attributes, input assembly (state only).
    PreRasterization, ///< Vertex/tessellation/geometry or task/mesh stages, viewport, raster.
    FragmentShader,   ///< Fragment stage, multisample, depth/stencil.
    FragmentOutput,   ///< Colour blend, multisample (state only).
};
inline constexpr size_t NumParts = 4;

[[nodiscard]] constexpr std::string_view PartName(Part part) noexcept {
    switch (part) {
    case Part::VertexInput:
        return "vertex_input";
    case Part::PreRasterization:
        return "pre_rasterization";
    case Part::FragmentShader:
        return "fragment_shader";
    case Part::FragmentOutput:
        return "fragment_output";
    }
    return "unknown";
}

[[nodiscard]] constexpr VkGraphicsPipelineLibraryFlagBitsEXT PartFlag(Part part) noexcept {
    switch (part) {
    case Part::VertexInput:
        return VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT;
    case Part::PreRasterization:
        return VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT;
    case Part::FragmentShader:
        return VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT;
    case Part::FragmentOutput:
        return VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT;
    }
    return VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT;
}

/// Why a pipeline is built monolithic although libraries are enabled.
enum class Unsupported : u8 {
    None,
    CreateFlags,             ///< Flags other than the descriptor buffer bit (statistics capture).
    UnknownChain,            ///< A pNext structure, specialization or array the split does not know.
    StaticRasterizerDiscard, ///< Rasterization statically off: no fragment parts to link.
    MissingModuleHash,       ///< A shader module whose SPIR-V the registry did not see.
    Count,
};

[[nodiscard]] constexpr std::string_view UnsupportedName(Unsupported reason) noexcept {
    switch (reason) {
    case Unsupported::None:
        return "none";
    case Unsupported::CreateFlags:
        return "create_flags";
    case Unsupported::UnknownChain:
        return "unknown_chain";
    case Unsupported::StaticRasterizerDiscard:
        return "static_rasterizer_discard";
    case Unsupported::MissingModuleHash:
        return "missing_module_hash";
    case Unsupported::Count:
        break;
    }
    return "unknown";
}

struct KeyHash {
    size_t operator()(const u128& key) const noexcept {
        return static_cast<size_t>(key[0] ^ (key[1] * 0x9E3779B97F4A7C15ULL));
    }
};

/// Serializes plain values for a key. Never takes pointers: chained structures are written
/// field by field by the caller.
class KeyWriter {
public:
    explicit KeyWriter(u32 tag) {
        Put(tag);
    }

    template <typename T>
    void Put(const T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        if constexpr (std::is_floating_point_v<T>) {
            static_assert(sizeof(T) == sizeof(u32));
            Put(std::bit_cast<u32>(value));
        } else {
            static_assert(std::has_unique_object_representations_v<T>);
            const auto* const data = reinterpret_cast<const u8*>(&value);
            bytes.insert(bytes.end(), data, data + sizeof(T));
        }
    }

    void PutString(std::string_view text) {
        Put(static_cast<u32>(text.size()));
        bytes.insert(bytes.end(), text.begin(), text.end());
    }

    [[nodiscard]] u128 Finish() const {
        return Common::CityHash128(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }

private:
    std::vector<u8> bytes;
};

/// What the split needs besides the create info.
struct Inputs {
    /// The SPIR-V content hash of each pStages[i].module, in pStages order.
    std::span<const u128> stage_modules;
    /// Equal for identically defined pipeline layouts (the libraries are made with a layout
    /// the library cache owns, the linked pipeline with the pipeline's own).
    u128 layout_identity{};
    /// The render pass. RenderPassCache keeps every render pass for the process, so the handle
    /// is a stable identity of its attachment formats and sample count.
    u64 render_pass_identity{};
};

/// One monolithic VkGraphicsPipelineCreateInfo, copied, with static values that dynamic state
/// overrides reset to a canonical value, and split into the four library parts.
///
/// The copy points into itself: construct in place, never move.
class Plan {
public:
    Plan() = default;
    Plan(const Plan&) = delete;
    Plan& operator=(const Plan&) = delete;

    /// False when the pipeline cannot be split (Reason says why); it is then built monolithic.
    bool Build(const VkGraphicsPipelineCreateInfo& ci, const Inputs& inputs) {
        reason = Split(ci, inputs);
        if (reason != Unsupported::None) {
            return false;
        }
        Normalize();
        Link();
        MakeKeys(inputs);
        return true;
    }

    [[nodiscard]] Unsupported Reason() const noexcept {
        return reason;
    }

    /// Mesh pipelines have no vertex input interface part.
    [[nodiscard]] bool HasPart(Part part) const noexcept {
        return part != Part::VertexInput || has_vertex_input;
    }

    [[nodiscard]] bool IsMesh() const noexcept {
        return !has_vertex_input;
    }

    [[nodiscard]] const u128& Key(Part part) const noexcept {
        return keys[static_cast<size_t>(part)];
    }

    /// The create flags of every library and of the link (the descriptor buffer bit, if any).
    [[nodiscard]] VkPipelineCreateFlags Flags() const noexcept {
        return flags;
    }

    [[nodiscard]] VkRenderPass RenderPass() const noexcept {
        return render_pass;
    }

    [[nodiscard]] u32 Subpass() const noexcept {
        return subpass;
    }

    /// The create info of one library part. layout is used by the shader parts only.
    [[nodiscard]] VkGraphicsPipelineCreateInfo LibraryInfo(Part part, VkPipelineLayout layout) {
        auto& library_ci = library_cis[static_cast<size_t>(part)];
        library_ci = {
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT,
            .pNext = nullptr,
            .flags = static_cast<VkGraphicsPipelineLibraryFlagsEXT>(PartFlag(part)),
        };
        VkGraphicsPipelineCreateInfo ci{
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .pNext = &library_ci,
            .flags = flags | VK_PIPELINE_CREATE_LIBRARY_BIT_KHR,
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
            .pDynamicState = &dynamic_ci,
            .layout = VK_NULL_HANDLE,
            .renderPass = VK_NULL_HANDLE,
            .subpass = 0,
            .basePipelineHandle = VK_NULL_HANDLE,
            .basePipelineIndex = 0,
        };
        switch (part) {
        case Part::VertexInput:
            ci.pVertexInputState = &vertex_input;
            ci.pInputAssemblyState = &input_assembly;
            break;
        case Part::PreRasterization:
            ci.stageCount = static_cast<u32>(pre_stages.size());
            ci.pStages = pre_stages.data();
            ci.pTessellationState = has_tessellation ? &tessellation : nullptr;
            ci.pViewportState = &viewport;
            ci.pRasterizationState = &rasterization;
            ci.layout = layout;
            ci.renderPass = render_pass;
            ci.subpass = subpass;
            break;
        case Part::FragmentShader:
            ci.stageCount = has_fragment_stage ? 1U : 0U;
            ci.pStages = has_fragment_stage ? &fragment_stage : nullptr;
            ci.pMultisampleState = &multisample;
            ci.pDepthStencilState = &depth_stencil;
            ci.layout = layout;
            ci.renderPass = render_pass;
            ci.subpass = subpass;
            break;
        case Part::FragmentOutput:
            ci.pMultisampleState = &multisample;
            ci.pColorBlendState = &color_blend;
            ci.renderPass = render_pass;
            ci.subpass = subpass;
            break;
        }
        return ci;
    }

    // The normalized copies, read by the tests.
    [[nodiscard]] const VkPipelineRasterizationStateCreateInfo& Rasterization() const noexcept {
        return rasterization;
    }
    [[nodiscard]] const VkPipelineDepthStencilStateCreateInfo& DepthStencil() const noexcept {
        return depth_stencil;
    }
    [[nodiscard]] const VkPipelineInputAssemblyStateCreateInfo& InputAssembly() const noexcept {
        return input_assembly;
    }
    [[nodiscard]] std::span<const VkPipelineColorBlendAttachmentState> Attachments()
        const noexcept {
        return attachments;
    }
    [[nodiscard]] std::span<const VkVertexInputBindingDescription> VertexBindings()
        const noexcept {
        return vertex_bindings;
    }

private:
    static constexpr u32 Tag(Part part) noexcept {
        return 0x47504C00u | static_cast<u32>(part); // "GPL" + part
    }

    [[nodiscard]] bool IsDynamic(VkDynamicState state) const noexcept {
        for (const VkDynamicState dynamic : dynamic_states) {
            if (dynamic == state) {
                return true;
            }
        }
        return false;
    }

    static bool IsFragment(VkShaderStageFlagBits stage) noexcept {
        return stage == VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    static bool IsTessellation(VkShaderStageFlagBits stage) noexcept {
        return stage == VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT ||
               stage == VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    }

    static bool IsMeshStage(VkShaderStageFlagBits stage) noexcept {
        return stage == VK_SHADER_STAGE_TASK_BIT_EXT || stage == VK_SHADER_STAGE_MESH_BIT_EXT;
    }

    Unsupported Split(const VkGraphicsPipelineCreateInfo& ci, const Inputs& inputs) {
        if (ci.pNext != nullptr) {
            return Unsupported::UnknownChain;
        }
        if ((ci.flags & ~VkPipelineCreateFlags{VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT}) !=
            0) {
            return Unsupported::CreateFlags;
        }
        flags = ci.flags;
        render_pass = ci.renderPass;
        subpass = ci.subpass;
        if (ci.pDynamicState != nullptr) {
            if (ci.pDynamicState->pNext != nullptr || ci.pDynamicState->flags != 0) {
                return Unsupported::UnknownChain;
            }
            dynamic_states.assign(ci.pDynamicState->pDynamicStates,
                                  ci.pDynamicState->pDynamicStates +
                                      ci.pDynamicState->dynamicStateCount);
        }
        if (inputs.stage_modules.size() != ci.stageCount) {
            return Unsupported::MissingModuleHash;
        }
        bool has_tessellation_stage = false;
        bool has_mesh_stage = false;
        for (u32 index = 0; index < ci.stageCount; ++index) {
            const VkPipelineShaderStageCreateInfo& stage = ci.pStages[index];
            if (stage.pNext != nullptr || stage.pSpecializationInfo != nullptr ||
                stage.flags != 0 || stage.pName == nullptr) {
                return Unsupported::UnknownChain;
            }
            if (IsFragment(stage.stage)) {
                fragment_stage = stage;
                fragment_module = inputs.stage_modules[index];
                has_fragment_stage = true;
            } else {
                pre_stages.push_back(stage);
                pre_modules.push_back(inputs.stage_modules[index]);
            }
            has_tessellation_stage |= IsTessellation(stage.stage);
            has_mesh_stage |= IsMeshStage(stage.stage);
        }
        if (pre_stages.empty()) {
            return Unsupported::UnknownChain;
        }

        has_vertex_input = !has_mesh_stage;
        if (has_vertex_input) {
            if (ci.pVertexInputState == nullptr || ci.pInputAssemblyState == nullptr ||
                ci.pInputAssemblyState->pNext != nullptr) {
                return Unsupported::UnknownChain;
            }
            vertex_input = *ci.pVertexInputState;
            vertex_bindings.assign(vertex_input.pVertexBindingDescriptions,
                                   vertex_input.pVertexBindingDescriptions +
                                       vertex_input.vertexBindingDescriptionCount);
            vertex_attributes.assign(vertex_input.pVertexAttributeDescriptions,
                                     vertex_input.pVertexAttributeDescriptions +
                                         vertex_input.vertexAttributeDescriptionCount);
            for (const auto* next = static_cast<const VkBaseInStructure*>(vertex_input.pNext);
                 next != nullptr; next = next->pNext) {
                if (next->sType !=
                    VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT) {
                    return Unsupported::UnknownChain;
                }
                divisor = *reinterpret_cast<const VkPipelineVertexInputDivisorStateCreateInfoEXT*>(
                    next);
                divisors.assign(divisor.pVertexBindingDivisors,
                                divisor.pVertexBindingDivisors + divisor.vertexBindingDivisorCount);
                has_divisor = true;
            }
            input_assembly = *ci.pInputAssemblyState;
        }
        if (has_tessellation_stage && ci.pTessellationState != nullptr) {
            if (ci.pTessellationState->pNext != nullptr) {
                return Unsupported::UnknownChain;
            }
            tessellation = *ci.pTessellationState;
            has_tessellation = true;
        }

        if (ci.pViewportState == nullptr || ci.pViewportState->pViewports != nullptr ||
            ci.pViewportState->pScissors != nullptr) {
            return Unsupported::UnknownChain;
        }
        viewport = *ci.pViewportState;
        for (const auto* next = static_cast<const VkBaseInStructure*>(viewport.pNext);
             next != nullptr; next = next->pNext) {
            switch (next->sType) {
            case VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_SWIZZLE_STATE_CREATE_INFO_NV:
                swizzle = *reinterpret_cast<const VkPipelineViewportSwizzleStateCreateInfoNV*>(next);
                swizzles.assign(swizzle.pViewportSwizzles,
                                swizzle.pViewportSwizzles + swizzle.viewportCount);
                has_swizzle = true;
                break;
            case VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT:
                depth_clip_control =
                    *reinterpret_cast<const VkPipelineViewportDepthClipControlCreateInfoEXT*>(next);
                has_depth_clip_control = true;
                break;
            default:
                return Unsupported::UnknownChain;
            }
        }

        if (ci.pRasterizationState == nullptr) {
            return Unsupported::UnknownChain;
        }
        rasterization = *ci.pRasterizationState;
        for (const auto* next = static_cast<const VkBaseInStructure*>(rasterization.pNext);
             next != nullptr; next = next->pNext) {
            switch (next->sType) {
            case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_LINE_STATE_CREATE_INFO_EXT:
                line = *reinterpret_cast<const VkPipelineRasterizationLineStateCreateInfoEXT*>(next);
                has_line = true;
                break;
            case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT:
                conservative = *reinterpret_cast<
                    const VkPipelineRasterizationConservativeStateCreateInfoEXT*>(next);
                has_conservative = true;
                break;
            case VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT:
                provoking = *reinterpret_cast<
                    const VkPipelineRasterizationProvokingVertexStateCreateInfoEXT*>(next);
                has_provoking = true;
                break;
            default:
                return Unsupported::UnknownChain;
            }
        }
        if (rasterization.rasterizerDiscardEnable != VK_FALSE &&
            !IsDynamic(VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE)) {
            return Unsupported::StaticRasterizerDiscard;
        }

        if (ci.pMultisampleState == nullptr || ci.pMultisampleState->pNext != nullptr ||
            ci.pMultisampleState->pSampleMask != nullptr || ci.pDepthStencilState == nullptr ||
            ci.pDepthStencilState->pNext != nullptr || ci.pColorBlendState == nullptr ||
            ci.pColorBlendState->pNext != nullptr) {
            return Unsupported::UnknownChain;
        }
        multisample = *ci.pMultisampleState;
        depth_stencil = *ci.pDepthStencilState;
        color_blend = *ci.pColorBlendState;
        attachments.assign(color_blend.pAttachments,
                           color_blend.pAttachments + color_blend.attachmentCount);
        return Unsupported::None;
    }

    /// Static values the dynamic state overrides at draw time get one canonical value, so that
    /// pipelines differing only in them share libraries (the driver ignores them anyway).
    void Normalize() {
        if (has_vertex_input) {
            if (IsDynamic(VK_DYNAMIC_STATE_VERTEX_INPUT_EXT)) {
                vertex_bindings.clear();
                vertex_attributes.clear();
                divisors.clear();
                has_divisor = false;
            } else if (IsDynamic(VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE_EXT)) {
                for (VkVertexInputBindingDescription& binding : vertex_bindings) {
                    binding.stride = 0;
                }
            }
            const bool restart_dynamic = IsDynamic(VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE);
            if (restart_dynamic) {
                input_assembly.primitiveRestartEnable = VK_FALSE;
            }
            // A dynamic topology must stay in the static topology's class; a list topology
            // with restart enabled statically is only valid as given, so it is kept then.
            if (IsDynamic(VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY) &&
                input_assembly.primitiveRestartEnable == VK_FALSE) {
                input_assembly.topology = TopologyClass(input_assembly.topology);
            }
        }
        if (IsDynamic(VK_DYNAMIC_STATE_CULL_MODE)) {
            rasterization.cullMode = VK_CULL_MODE_NONE;
        }
        if (IsDynamic(VK_DYNAMIC_STATE_FRONT_FACE)) {
            rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        }
        if (IsDynamic(VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE)) {
            rasterization.depthBiasEnable = VK_FALSE;
        }
        if (IsDynamic(VK_DYNAMIC_STATE_DEPTH_BIAS)) {
            rasterization.depthBiasConstantFactor = 0.0f;
            rasterization.depthBiasClamp = 0.0f;
            rasterization.depthBiasSlopeFactor = 0.0f;
        }
        if (IsDynamic(VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE)) {
            rasterization.rasterizerDiscardEnable = VK_FALSE;
        }
        if (IsDynamic(VK_DYNAMIC_STATE_LINE_WIDTH)) {
            rasterization.lineWidth = 1.0f;
        }
        if (IsDynamic(VK_DYNAMIC_STATE_DEPTH_CLAMP_ENABLE_EXT)) {
            rasterization.depthClampEnable = VK_FALSE;
        }
        if (has_line) {
            if (IsDynamic(VK_DYNAMIC_STATE_LINE_STIPPLE_ENABLE_EXT)) {
                line.stippledLineEnable = VK_FALSE;
            }
            if (IsDynamic(VK_DYNAMIC_STATE_LINE_RASTERIZATION_MODE_EXT) &&
                line.stippledLineEnable == VK_FALSE) {
                line.lineRasterizationMode = VK_LINE_RASTERIZATION_MODE_DEFAULT_EXT;
            }
            if (IsDynamic(VK_DYNAMIC_STATE_LINE_STIPPLE_EXT)) {
                line.lineStippleFactor = 0;
                line.lineStipplePattern = 0;
            }
        }
        if (has_conservative && IsDynamic(VK_DYNAMIC_STATE_CONSERVATIVE_RASTERIZATION_MODE_EXT)) {
            conservative.conservativeRasterizationMode =
                VK_CONSERVATIVE_RASTERIZATION_MODE_DISABLED_EXT;
        }

        if (IsDynamic(VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE)) {
            depth_stencil.depthTestEnable = VK_FALSE;
        }
        if (IsDynamic(VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE)) {
            depth_stencil.depthWriteEnable = VK_FALSE;
        }
        if (IsDynamic(VK_DYNAMIC_STATE_DEPTH_COMPARE_OP)) {
            depth_stencil.depthCompareOp = VK_COMPARE_OP_NEVER;
        }
        if (IsDynamic(VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE)) {
            depth_stencil.depthBoundsTestEnable = VK_FALSE;
        }
        if (IsDynamic(VK_DYNAMIC_STATE_DEPTH_BOUNDS)) {
            depth_stencil.minDepthBounds = 0.0f;
            depth_stencil.maxDepthBounds = 0.0f;
        }
        if (IsDynamic(VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE)) {
            depth_stencil.stencilTestEnable = VK_FALSE;
        }
        for (VkStencilOpState* face : {&depth_stencil.front, &depth_stencil.back}) {
            if (IsDynamic(VK_DYNAMIC_STATE_STENCIL_OP)) {
                face->failOp = VK_STENCIL_OP_KEEP;
                face->passOp = VK_STENCIL_OP_KEEP;
                face->depthFailOp = VK_STENCIL_OP_KEEP;
                face->compareOp = VK_COMPARE_OP_NEVER;
            }
            if (IsDynamic(VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK)) {
                face->compareMask = 0;
            }
            if (IsDynamic(VK_DYNAMIC_STATE_STENCIL_WRITE_MASK)) {
                face->writeMask = 0;
            }
            if (IsDynamic(VK_DYNAMIC_STATE_STENCIL_REFERENCE)) {
                face->reference = 0;
            }
        }

        if (IsDynamic(VK_DYNAMIC_STATE_ALPHA_TO_COVERAGE_ENABLE_EXT)) {
            multisample.alphaToCoverageEnable = VK_FALSE;
        }
        if (IsDynamic(VK_DYNAMIC_STATE_ALPHA_TO_ONE_ENABLE_EXT)) {
            multisample.alphaToOneEnable = VK_FALSE;
        }

        if (IsDynamic(VK_DYNAMIC_STATE_LOGIC_OP_EXT)) {
            color_blend.logicOp = VK_LOGIC_OP_CLEAR;
        }
        if (IsDynamic(VK_DYNAMIC_STATE_LOGIC_OP_ENABLE_EXT)) {
            color_blend.logicOpEnable = VK_FALSE;
        }
        if (IsDynamic(VK_DYNAMIC_STATE_BLEND_CONSTANTS)) {
            color_blend.blendConstants[0] = 0.0f;
            color_blend.blendConstants[1] = 0.0f;
            color_blend.blendConstants[2] = 0.0f;
            color_blend.blendConstants[3] = 0.0f;
        }
        for (VkPipelineColorBlendAttachmentState& attachment : attachments) {
            if (IsDynamic(VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT)) {
                attachment.blendEnable = VK_FALSE;
            }
            if (IsDynamic(VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT)) {
                attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
                attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
                attachment.colorBlendOp = VK_BLEND_OP_ADD;
                attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
                attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
                attachment.alphaBlendOp = VK_BLEND_OP_ADD;
            }
            if (IsDynamic(VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT)) {
                attachment.colorWriteMask = 0;
            }
        }
    }

    static VkPrimitiveTopology TopologyClass(VkPrimitiveTopology topology) noexcept {
        switch (topology) {
        case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
            return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
        case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
        case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY:
        case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY:
            return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
        case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
        case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:
        case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY:
        case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY:
            return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        default:
            return topology;
        }
    }

    /// Points the copies at each other (after Normalize, which may resize the arrays).
    void Link() {
        dynamic_ci = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .dynamicStateCount = static_cast<u32>(dynamic_states.size()),
            .pDynamicStates = dynamic_states.data(),
        };
        if (has_vertex_input) {
            vertex_input.pNext = nullptr;
            vertex_input.vertexBindingDescriptionCount = static_cast<u32>(vertex_bindings.size());
            vertex_input.pVertexBindingDescriptions = vertex_bindings.data();
            vertex_input.vertexAttributeDescriptionCount =
                static_cast<u32>(vertex_attributes.size());
            vertex_input.pVertexAttributeDescriptions = vertex_attributes.data();
            if (has_divisor) {
                divisor.pNext = nullptr;
                divisor.vertexBindingDivisorCount = static_cast<u32>(divisors.size());
                divisor.pVertexBindingDivisors = divisors.data();
                vertex_input.pNext = &divisor;
            }
        }
        viewport.pNext = nullptr;
        if (has_swizzle) {
            swizzle.pNext = std::exchange(viewport.pNext, &swizzle);
            swizzle.viewportCount = static_cast<u32>(swizzles.size());
            swizzle.pViewportSwizzles = swizzles.data();
        }
        if (has_depth_clip_control) {
            depth_clip_control.pNext = std::exchange(viewport.pNext, &depth_clip_control);
        }
        rasterization.pNext = nullptr;
        if (has_line) {
            line.pNext = std::exchange(rasterization.pNext, &line);
        }
        if (has_conservative) {
            conservative.pNext = std::exchange(rasterization.pNext, &conservative);
        }
        if (has_provoking) {
            provoking.pNext = std::exchange(rasterization.pNext, &provoking);
        }
        color_blend.attachmentCount = static_cast<u32>(attachments.size());
        color_blend.pAttachments = attachments.data();
    }

    void PutCommon(KeyWriter& writer) const {
        writer.Put(flags);
        writer.Put(static_cast<u32>(dynamic_states.size()));
        for (const VkDynamicState state : dynamic_states) {
            writer.Put(state);
        }
    }

    static void PutStage(KeyWriter& writer, const VkPipelineShaderStageCreateInfo& stage,
                         const u128& module) {
        writer.Put(stage.stage);
        writer.Put(module);
        writer.PutString(stage.pName);
    }

    void PutMultisample(KeyWriter& writer) const {
        writer.Put(multisample.flags);
        writer.Put(multisample.rasterizationSamples);
        writer.Put(multisample.sampleShadingEnable);
        writer.Put(multisample.minSampleShading);
        writer.Put(multisample.alphaToCoverageEnable);
        writer.Put(multisample.alphaToOneEnable);
    }

    void MakeKeys(const Inputs& inputs) {
        {
            KeyWriter writer{Tag(Part::VertexInput)};
            PutCommon(writer);
            if (has_vertex_input) {
                writer.Put(vertex_input.flags);
                writer.Put(static_cast<u32>(vertex_bindings.size()));
                for (const auto& binding : vertex_bindings) {
                    writer.Put(binding.binding);
                    writer.Put(binding.stride);
                    writer.Put(binding.inputRate);
                }
                writer.Put(static_cast<u32>(vertex_attributes.size()));
                for (const auto& attribute : vertex_attributes) {
                    writer.Put(attribute.location);
                    writer.Put(attribute.binding);
                    writer.Put(attribute.format);
                    writer.Put(attribute.offset);
                }
                writer.Put(static_cast<u32>(has_divisor ? divisors.size() : 0));
                for (const auto& entry : divisors) {
                    writer.Put(entry.binding);
                    writer.Put(entry.divisor);
                }
                writer.Put(input_assembly.flags);
                writer.Put(input_assembly.topology);
                writer.Put(input_assembly.primitiveRestartEnable);
            }
            keys[static_cast<size_t>(Part::VertexInput)] = writer.Finish();
        }
        {
            KeyWriter writer{Tag(Part::PreRasterization)};
            PutCommon(writer);
            writer.Put(inputs.layout_identity);
            writer.Put(inputs.render_pass_identity);
            writer.Put(subpass);
            writer.Put(static_cast<u32>(pre_stages.size()));
            for (size_t index = 0; index < pre_stages.size(); ++index) {
                PutStage(writer, pre_stages[index], pre_modules[index]);
            }
            writer.Put(static_cast<u32>(has_tessellation));
            if (has_tessellation) {
                writer.Put(tessellation.flags);
                writer.Put(tessellation.patchControlPoints);
            }
            writer.Put(viewport.flags);
            writer.Put(viewport.viewportCount);
            writer.Put(viewport.scissorCount);
            writer.Put(static_cast<u32>(has_swizzle ? swizzles.size() : 0));
            for (const auto& entry : swizzles) {
                writer.Put(entry.x);
                writer.Put(entry.y);
                writer.Put(entry.z);
                writer.Put(entry.w);
            }
            writer.Put(static_cast<u32>(has_depth_clip_control));
            if (has_depth_clip_control) {
                writer.Put(depth_clip_control.negativeOneToOne);
            }
            writer.Put(rasterization.flags);
            writer.Put(rasterization.depthClampEnable);
            writer.Put(rasterization.rasterizerDiscardEnable);
            writer.Put(rasterization.polygonMode);
            writer.Put(rasterization.cullMode);
            writer.Put(rasterization.frontFace);
            writer.Put(rasterization.depthBiasEnable);
            writer.Put(rasterization.depthBiasConstantFactor);
            writer.Put(rasterization.depthBiasClamp);
            writer.Put(rasterization.depthBiasSlopeFactor);
            writer.Put(rasterization.lineWidth);
            writer.Put(static_cast<u32>(has_line));
            if (has_line) {
                writer.Put(line.lineRasterizationMode);
                writer.Put(line.stippledLineEnable);
                writer.Put(line.lineStippleFactor);
                writer.Put(line.lineStipplePattern);
            }
            writer.Put(static_cast<u32>(has_conservative));
            if (has_conservative) {
                writer.Put(conservative.flags);
                writer.Put(conservative.conservativeRasterizationMode);
                writer.Put(conservative.extraPrimitiveOverestimationSize);
            }
            writer.Put(static_cast<u32>(has_provoking));
            if (has_provoking) {
                writer.Put(provoking.provokingVertexMode);
            }
            keys[static_cast<size_t>(Part::PreRasterization)] = writer.Finish();
        }
        {
            KeyWriter writer{Tag(Part::FragmentShader)};
            PutCommon(writer);
            writer.Put(inputs.layout_identity);
            writer.Put(inputs.render_pass_identity);
            writer.Put(subpass);
            writer.Put(static_cast<u32>(has_fragment_stage));
            if (has_fragment_stage) {
                PutStage(writer, fragment_stage, fragment_module);
            }
            PutMultisample(writer);
            writer.Put(depth_stencil.flags);
            writer.Put(depth_stencil.depthTestEnable);
            writer.Put(depth_stencil.depthWriteEnable);
            writer.Put(depth_stencil.depthCompareOp);
            writer.Put(depth_stencil.depthBoundsTestEnable);
            writer.Put(depth_stencil.stencilTestEnable);
            for (const VkStencilOpState& face : {depth_stencil.front, depth_stencil.back}) {
                writer.Put(face.failOp);
                writer.Put(face.passOp);
                writer.Put(face.depthFailOp);
                writer.Put(face.compareOp);
                writer.Put(face.compareMask);
                writer.Put(face.writeMask);
                writer.Put(face.reference);
            }
            writer.Put(depth_stencil.minDepthBounds);
            writer.Put(depth_stencil.maxDepthBounds);
            keys[static_cast<size_t>(Part::FragmentShader)] = writer.Finish();
        }
        {
            KeyWriter writer{Tag(Part::FragmentOutput)};
            PutCommon(writer);
            writer.Put(inputs.render_pass_identity);
            writer.Put(subpass);
            PutMultisample(writer);
            writer.Put(color_blend.flags);
            writer.Put(color_blend.logicOpEnable);
            writer.Put(color_blend.logicOp);
            writer.Put(static_cast<u32>(attachments.size()));
            for (const auto& attachment : attachments) {
                writer.Put(attachment.blendEnable);
                writer.Put(attachment.srcColorBlendFactor);
                writer.Put(attachment.dstColorBlendFactor);
                writer.Put(attachment.colorBlendOp);
                writer.Put(attachment.srcAlphaBlendFactor);
                writer.Put(attachment.dstAlphaBlendFactor);
                writer.Put(attachment.alphaBlendOp);
                writer.Put(attachment.colorWriteMask);
            }
            for (const float constant : color_blend.blendConstants) {
                writer.Put(constant);
            }
            keys[static_cast<size_t>(Part::FragmentOutput)] = writer.Finish();
        }
    }

    Unsupported reason{Unsupported::None};
    std::array<u128, NumParts> keys{};
    std::array<VkGraphicsPipelineLibraryCreateInfoEXT, NumParts> library_cis{};
    VkPipelineCreateFlags flags{};
    VkRenderPass render_pass{};
    u32 subpass{};

    std::vector<VkDynamicState> dynamic_states;
    VkPipelineDynamicStateCreateInfo dynamic_ci{};

    std::vector<VkPipelineShaderStageCreateInfo> pre_stages;
    std::vector<u128> pre_modules;
    VkPipelineShaderStageCreateInfo fragment_stage{};
    u128 fragment_module{};
    bool has_fragment_stage{};

    bool has_vertex_input{};
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    std::vector<VkVertexInputBindingDescription> vertex_bindings;
    std::vector<VkVertexInputAttributeDescription> vertex_attributes;
    VkPipelineVertexInputDivisorStateCreateInfoEXT divisor{};
    std::vector<VkVertexInputBindingDivisorDescriptionEXT> divisors;
    bool has_divisor{};
    VkPipelineInputAssemblyStateCreateInfo input_assembly{};

    VkPipelineTessellationStateCreateInfo tessellation{};
    bool has_tessellation{};

    VkPipelineViewportStateCreateInfo viewport{};
    VkPipelineViewportSwizzleStateCreateInfoNV swizzle{};
    std::vector<VkViewportSwizzleNV> swizzles;
    bool has_swizzle{};
    VkPipelineViewportDepthClipControlCreateInfoEXT depth_clip_control{};
    bool has_depth_clip_control{};

    VkPipelineRasterizationStateCreateInfo rasterization{};
    VkPipelineRasterizationLineStateCreateInfoEXT line{};
    bool has_line{};
    VkPipelineRasterizationConservativeStateCreateInfoEXT conservative{};
    bool has_conservative{};
    VkPipelineRasterizationProvokingVertexStateCreateInfoEXT provoking{};
    bool has_provoking{};

    VkPipelineMultisampleStateCreateInfo multisample{};
    VkPipelineDepthStencilStateCreateInfo depth_stencil{};
    VkPipelineColorBlendStateCreateInfo color_blend{};
    std::vector<VkPipelineColorBlendAttachmentState> attachments;
};

} // namespace Vulkan::PipelineLibrary
