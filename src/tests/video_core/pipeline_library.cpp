// SPDX-License-Identifier: GPL-3.0-or-later

// The library split of EDEN_NX2_PIPELINE_LIBRARY=1: which part of a monolithic create info each
// library key depends on, and what the split refuses.

#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/pipeline_library.h"

namespace {
using Vulkan::PipelineLibrary::Inputs;
using Vulkan::PipelineLibrary::Part;
using Vulkan::PipelineLibrary::Plan;
using Vulkan::PipelineLibrary::Unsupported;

constexpr std::array AllParts{Part::VertexInput, Part::PreRasterization, Part::FragmentShader,
                              Part::FragmentOutput};

VkShaderModule FakeModule(uintptr_t value) {
    return reinterpret_cast<VkShaderModule>(value);
}

VkRenderPass FakeRenderPass(uintptr_t value) {
    return reinterpret_cast<VkRenderPass>(value);
}

/// A monolithic create info built like GraphicsPipeline::MakePipeline builds one.
struct Pipeline {
    std::vector<VkDynamicState> dynamic_states{
        VK_DYNAMIC_STATE_VIEWPORT,           VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_DEPTH_BIAS,         VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        VK_DYNAMIC_STATE_DEPTH_BOUNDS,       VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        VK_DYNAMIC_STATE_LINE_WIDTH,
    };
    std::vector<VkVertexInputBindingDescription> bindings{{0, 32, VK_VERTEX_INPUT_RATE_VERTEX}};
    std::vector<VkVertexInputAttributeDescription> attributes{
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 0, VK_FORMAT_R8G8B8A8_UNORM, 12}};
    VkPrimitiveTopology topology{VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkBool32 primitive_restart{VK_FALSE};
    VkCullModeFlags cull_mode{VK_CULL_MODE_BACK_BIT};
    VkBool32 rasterizer_discard{VK_FALSE};
    VkBool32 depth_test{VK_TRUE};
    VkSampleCountFlagBits samples{VK_SAMPLE_COUNT_1_BIT};
    VkBool32 blend_enable{VK_FALSE};
    std::vector<VkShaderStageFlagBits> stage_bits{VK_SHADER_STAGE_VERTEX_BIT,
                                                  VK_SHADER_STAGE_FRAGMENT_BIT};
    std::vector<u128> modules{u128{1, 2}, u128{3, 4}};
    u128 layout_identity{5, 6};
    VkRenderPass render_pass{FakeRenderPass(0x1000)};
    VkPipelineCreateFlags flags{};
    const void* rasterization_next{};

    void UseDynamicState() {
        // EDS1 + EDS2 as the session keys have them (dyn67).
        dynamic_states.insert(dynamic_states.end(),
                              {VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE,
                               VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
                               VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
                               VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
                               VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE,
                               VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP,
                               VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
                               VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
                               VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE,
                               VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE});
    }

    void UseDynamicVertexInput() {
        dynamic_states.push_back(VK_DYNAMIC_STATE_VERTEX_INPUT_EXT);
        bindings.clear();
        attributes.clear();
    }

    /// Builds the plan from create infos that live only during the call, as in MakePipeline.
    bool Build(Plan& plan) const {
        std::vector<VkPipelineShaderStageCreateInfo> stages;
        for (size_t index = 0; index < stage_bits.size(); ++index) {
            stages.push_back({
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .stage = stage_bits[index],
                .module = FakeModule(0x100 + index),
                .pName = "main",
                .pSpecializationInfo = nullptr,
            });
        }
        const bool is_mesh = stage_bits[0] == VK_SHADER_STAGE_MESH_BIT_EXT ||
                             stage_bits[0] == VK_SHADER_STAGE_TASK_BIT_EXT;
        const VkPipelineVertexInputStateCreateInfo vertex_input{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .vertexBindingDescriptionCount = static_cast<u32>(bindings.size()),
            .pVertexBindingDescriptions = bindings.data(),
            .vertexAttributeDescriptionCount = static_cast<u32>(attributes.size()),
            .pVertexAttributeDescriptions = attributes.data(),
        };
        const VkPipelineInputAssemblyStateCreateInfo input_assembly{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .topology = topology,
            .primitiveRestartEnable = primitive_restart,
        };
        const VkPipelineTessellationStateCreateInfo tessellation{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .patchControlPoints = 1,
        };
        const VkPipelineViewportDepthClipControlCreateInfoEXT depth_clip{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT,
            .pNext = nullptr,
            .negativeOneToOne = VK_FALSE,
        };
        const VkPipelineViewportStateCreateInfo viewport{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .pNext = &depth_clip,
            .flags = 0,
            .viewportCount = 16,
            .pViewports = nullptr,
            .scissorCount = 16,
            .pScissors = nullptr,
        };
        const VkPipelineRasterizationProvokingVertexStateCreateInfoEXT provoking{
            .sType =
                VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT,
            .pNext = rasterization_next,
            .provokingVertexMode = VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT,
        };
        const VkPipelineRasterizationStateCreateInfo rasterization{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .pNext = &provoking,
            .flags = 0,
            .depthClampEnable = VK_TRUE,
            .rasterizerDiscardEnable = rasterizer_discard,
            .polygonMode = VK_POLYGON_MODE_FILL,
            .cullMode = cull_mode,
            .frontFace = VK_FRONT_FACE_CLOCKWISE,
            .depthBiasEnable = VK_FALSE,
            .depthBiasConstantFactor = 0.0f,
            .depthBiasClamp = 0.0f,
            .depthBiasSlopeFactor = 0.0f,
            .lineWidth = 1.0f,
        };
        const VkPipelineMultisampleStateCreateInfo multisample{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .rasterizationSamples = samples,
            .sampleShadingEnable = VK_FALSE,
            .minSampleShading = 0.0f,
            .pSampleMask = nullptr,
            .alphaToCoverageEnable = VK_FALSE,
            .alphaToOneEnable = VK_FALSE,
        };
        const VkPipelineDepthStencilStateCreateInfo depth_stencil{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .depthTestEnable = depth_test,
            .depthWriteEnable = VK_TRUE,
            .depthCompareOp = VK_COMPARE_OP_LESS,
            .depthBoundsTestEnable = VK_FALSE,
            .stencilTestEnable = VK_FALSE,
            .front = {},
            .back = {},
            .minDepthBounds = 0.25f,
            .maxDepthBounds = 0.75f,
        };
        const VkPipelineColorBlendAttachmentState attachment{
            .blendEnable = blend_enable,
            .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
            .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            .colorBlendOp = VK_BLEND_OP_ADD,
            .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
            .alphaBlendOp = VK_BLEND_OP_ADD,
            .colorWriteMask = 0xF,
        };
        const VkPipelineColorBlendStateCreateInfo color_blend{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .logicOpEnable = VK_FALSE,
            .logicOp = VK_LOGIC_OP_COPY,
            .attachmentCount = 1,
            .pAttachments = &attachment,
            .blendConstants = {},
        };
        const VkPipelineDynamicStateCreateInfo dynamic{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .dynamicStateCount = static_cast<u32>(dynamic_states.size()),
            .pDynamicStates = dynamic_states.data(),
        };
        const VkGraphicsPipelineCreateInfo ci{
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .pNext = nullptr,
            .flags = flags,
            .stageCount = static_cast<u32>(stages.size()),
            .pStages = stages.data(),
            .pVertexInputState = is_mesh ? nullptr : &vertex_input,
            .pInputAssemblyState = is_mesh ? nullptr : &input_assembly,
            .pTessellationState = is_mesh ? nullptr : &tessellation,
            .pViewportState = &viewport,
            .pRasterizationState = &rasterization,
            .pMultisampleState = &multisample,
            .pDepthStencilState = &depth_stencil,
            .pColorBlendState = &color_blend,
            .pDynamicState = &dynamic,
            .layout = reinterpret_cast<VkPipelineLayout>(uintptr_t{0x2000}),
            .renderPass = render_pass,
            .subpass = 0,
            .basePipelineHandle = VK_NULL_HANDLE,
            .basePipelineIndex = 0,
        };
        const Inputs inputs{
            .stage_modules = modules,
            .layout_identity = layout_identity,
            .render_pass_identity = reinterpret_cast<u64>(render_pass),
        };
        return plan.Build(ci, inputs);
    }
};

/// The parts whose key differs between the two pipelines.
template <typename Change>
std::vector<Part> ChangedParts(const Pipeline& base, Change&& change) {
    Pipeline other = base;
    change(other);
    Plan before;
    Plan after;
    REQUIRE(base.Build(before));
    REQUIRE(other.Build(after));
    std::vector<Part> changed;
    for (const Part part : AllParts) {
        if (before.Key(part) != after.Key(part)) {
            changed.push_back(part);
        }
    }
    return changed;
}

using Parts = std::vector<Part>;
} // Anonymous namespace

TEST_CASE("PipelineLibrary: each piece of state keys only its own library", "[video_core]") {
    const Pipeline base;
    CHECK(ChangedParts(base, [](Pipeline&) {}).empty());
    CHECK(ChangedParts(base, [](Pipeline& p) { p.modules[1] = {7, 7}; }) ==
          Parts{Part::FragmentShader});
    CHECK(ChangedParts(base, [](Pipeline& p) { p.modules[0] = {7, 7}; }) ==
          Parts{Part::PreRasterization});
    CHECK(ChangedParts(base, [](Pipeline& p) { p.blend_enable = VK_TRUE; }) ==
          Parts{Part::FragmentOutput});
    CHECK(ChangedParts(base, [](Pipeline& p) { p.depth_test = VK_FALSE; }) ==
          Parts{Part::FragmentShader});
    CHECK(ChangedParts(base, [](Pipeline& p) { p.cull_mode = VK_CULL_MODE_NONE; }) ==
          Parts{Part::PreRasterization});
    CHECK(ChangedParts(base, [](Pipeline& p) { p.attributes[1].offset = 16; }) ==
          Parts{Part::VertexInput});
    CHECK(ChangedParts(base, [](Pipeline& p) {
              p.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
          }) == Parts{Part::VertexInput});
    // Sample count: the fragment shader and the output both see the multisample state.
    CHECK(ChangedParts(base, [](Pipeline& p) { p.samples = VK_SAMPLE_COUNT_4_BIT; }) ==
          Parts{Part::FragmentShader, Part::FragmentOutput});
    // The two shader libraries are made with the layout; the state-only ones are not.
    CHECK(ChangedParts(base, [](Pipeline& p) { p.layout_identity = {9, 9}; }) ==
          Parts{Part::PreRasterization, Part::FragmentShader});
    CHECK(ChangedParts(base, [](Pipeline& p) { p.render_pass = FakeRenderPass(0x1001); }) ==
          Parts{Part::PreRasterization, Part::FragmentShader, Part::FragmentOutput});
}

TEST_CASE("PipelineLibrary: static values of dynamic state do not split libraries",
          "[video_core]") {
    Pipeline base;
    base.UseDynamicState();
    CHECK(ChangedParts(base, [](Pipeline& p) { p.cull_mode = VK_CULL_MODE_NONE; }).empty());
    CHECK(ChangedParts(base, [](Pipeline& p) { p.depth_test = VK_FALSE; }).empty());
    CHECK(ChangedParts(base, [](Pipeline& p) { p.primitive_restart = VK_TRUE; }).empty());
    // Dynamic topology keeps the topology class.
    CHECK(ChangedParts(base, [](Pipeline& p) {
              p.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
          }).empty());
    CHECK(ChangedParts(base, [](Pipeline& p) { p.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; }) ==
          Parts{Part::VertexInput});
    // Rasterizer discard is dynamic with EDS2, so it is not a reason to stay monolithic.
    CHECK(ChangedParts(base, [](Pipeline& p) { p.rasterizer_discard = VK_TRUE; }).empty());
    // Blending is static without EDS3.
    CHECK(ChangedParts(base, [](Pipeline& p) { p.blend_enable = VK_TRUE; }) ==
          Parts{Part::FragmentOutput});

    Plan plan;
    REQUIRE(base.Build(plan));
    CHECK(plan.Rasterization().cullMode == VK_CULL_MODE_NONE);
    CHECK(plan.Rasterization().frontFace == VK_FRONT_FACE_COUNTER_CLOCKWISE);
    CHECK(plan.DepthStencil().depthTestEnable == VK_FALSE);
    CHECK(plan.DepthStencil().minDepthBounds == 0.0f);
    CHECK(plan.InputAssembly().topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
}

TEST_CASE("PipelineLibrary: dynamic vertex input and strides", "[video_core]") {
    Pipeline base;
    base.UseDynamicState();
    base.dynamic_states.push_back(VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE);
    CHECK(ChangedParts(base, [](Pipeline& p) { p.bindings[0].stride = 48; }).empty());
    CHECK(ChangedParts(base, [](Pipeline& p) { p.attributes[1].offset = 16; }) ==
          Parts{Part::VertexInput});
    Plan plan;
    REQUIRE(base.Build(plan));
    REQUIRE(plan.VertexBindings().size() == 1);
    CHECK(plan.VertexBindings()[0].stride == 0);

    Pipeline dynamic_input;
    dynamic_input.UseDynamicState();
    dynamic_input.UseDynamicVertexInput();
    Plan dynamic_plan;
    REQUIRE(dynamic_input.Build(dynamic_plan));
    CHECK(dynamic_plan.VertexBindings().empty());
}

TEST_CASE("PipelineLibrary: what stays monolithic", "[video_core]") {
    Plan plan;
    Pipeline statistics;
    statistics.flags = VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
    CHECK_FALSE(statistics.Build(plan));
    CHECK(plan.Reason() == Unsupported::CreateFlags);

    Plan discard_plan;
    Pipeline discard;
    discard.rasterizer_discard = VK_TRUE;
    CHECK_FALSE(discard.Build(discard_plan));
    CHECK(discard_plan.Reason() == Unsupported::StaticRasterizerDiscard);

    // A chained structure the split does not know is never dropped silently.
    const VkPipelineRasterizationStateStreamCreateInfoEXT stream{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_STREAM_CREATE_INFO_EXT,
        .pNext = nullptr,
        .flags = 0,
        .rasterizationStream = 0,
    };
    Plan chain_plan;
    Pipeline chain;
    chain.rasterization_next = &stream;
    CHECK_FALSE(chain.Build(chain_plan));
    CHECK(chain_plan.Reason() == Unsupported::UnknownChain);

    Plan missing_plan;
    Pipeline missing;
    missing.modules.pop_back();
    CHECK_FALSE(missing.Build(missing_plan));
    CHECK(missing_plan.Reason() == Unsupported::MissingModuleHash);

    Plan buffer_plan;
    Pipeline descriptor_buffer;
    descriptor_buffer.flags = VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT;
    REQUIRE(descriptor_buffer.Build(buffer_plan));
    for (const Part part : AllParts) {
        const VkGraphicsPipelineCreateInfo ci = buffer_plan.LibraryInfo(part, VK_NULL_HANDLE);
        CHECK((ci.flags & VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT) != 0);
        CHECK((ci.flags & VK_PIPELINE_CREATE_LIBRARY_BIT_KHR) != 0);
    }
}

TEST_CASE("PipelineLibrary: library create infos", "[video_core]") {
    Pipeline base;
    base.UseDynamicState();
    Plan plan;
    REQUIRE(base.Build(plan));
    const VkPipelineLayout layout = reinterpret_cast<VkPipelineLayout>(uintptr_t{0x3000});
    for (const Part part : AllParts) {
        const VkGraphicsPipelineCreateInfo ci = plan.LibraryInfo(part, layout);
        const auto* library = static_cast<const VkGraphicsPipelineLibraryCreateInfoEXT*>(ci.pNext);
        REQUIRE(library != nullptr);
        CHECK(library->sType == VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT);
        CHECK(library->flags == static_cast<VkGraphicsPipelineLibraryFlagsEXT>(
                                    Vulkan::PipelineLibrary::PartFlag(part)));
        REQUIRE(ci.pDynamicState != nullptr);
        CHECK(ci.pDynamicState->dynamicStateCount == base.dynamic_states.size());
        switch (part) {
        case Part::VertexInput:
            CHECK(ci.stageCount == 0);
            CHECK(ci.layout == VK_NULL_HANDLE);
            CHECK(ci.renderPass == VK_NULL_HANDLE);
            REQUIRE(ci.pVertexInputState != nullptr);
            CHECK(ci.pVertexInputState->vertexAttributeDescriptionCount == 2);
            break;
        case Part::PreRasterization:
            REQUIRE(ci.stageCount == 1);
            CHECK(ci.pStages[0].stage == VK_SHADER_STAGE_VERTEX_BIT);
            CHECK(ci.layout == layout);
            CHECK(ci.renderPass == base.render_pass);
            CHECK(ci.pTessellationState == nullptr);
            REQUIRE(ci.pRasterizationState != nullptr);
            REQUIRE(ci.pRasterizationState->pNext != nullptr);
            CHECK(static_cast<const VkBaseInStructure*>(ci.pRasterizationState->pNext)->sType ==
                  VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT);
            REQUIRE(ci.pViewportState != nullptr);
            CHECK(ci.pViewportState->pNext != nullptr);
            break;
        case Part::FragmentShader:
            REQUIRE(ci.stageCount == 1);
            CHECK(ci.pStages[0].stage == VK_SHADER_STAGE_FRAGMENT_BIT);
            CHECK(ci.layout == layout);
            CHECK(ci.pDepthStencilState != nullptr);
            CHECK(ci.pMultisampleState != nullptr);
            break;
        case Part::FragmentOutput:
            CHECK(ci.stageCount == 0);
            CHECK(ci.layout == VK_NULL_HANDLE);
            REQUIRE(ci.pColorBlendState != nullptr);
            CHECK(ci.pColorBlendState->attachmentCount == 1);
            CHECK(ci.pMultisampleState != nullptr);
            break;
        }
    }
}

TEST_CASE("PipelineLibrary: mesh pipelines have no vertex input part", "[video_core]") {
    Pipeline mesh;
    mesh.UseDynamicState();
    mesh.stage_bits = {VK_SHADER_STAGE_TASK_BIT_EXT, VK_SHADER_STAGE_MESH_BIT_EXT,
                       VK_SHADER_STAGE_FRAGMENT_BIT};
    mesh.modules = {u128{1, 1}, u128{2, 2}, u128{3, 3}};
    Plan plan;
    REQUIRE(mesh.Build(plan));
    CHECK(plan.IsMesh());
    CHECK_FALSE(plan.HasPart(Part::VertexInput));
    const VkGraphicsPipelineCreateInfo pre = plan.LibraryInfo(Part::PreRasterization, VK_NULL_HANDLE);
    REQUIRE(pre.stageCount == 2);
    CHECK(pre.pStages[0].stage == VK_SHADER_STAGE_TASK_BIT_EXT);
    CHECK(pre.pStages[1].stage == VK_SHADER_STAGE_MESH_BIT_EXT);

    // Vertex-only pipelines (depth-only passes) get a fragment shader part without a stage.
    Pipeline depth_only;
    depth_only.stage_bits = {VK_SHADER_STAGE_VERTEX_BIT};
    depth_only.modules = {u128{1, 1}};
    Plan depth_plan;
    REQUIRE(depth_only.Build(depth_plan));
    CHECK(depth_plan.LibraryInfo(Part::FragmentShader, VK_NULL_HANDLE).stageCount == 0);
}
