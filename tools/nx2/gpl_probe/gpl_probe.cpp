// SPDX-License-Identifier: GPL-3.0-or-later

// Standalone measurement of VK_EXT_graphics_pipeline_library on the local driver, with the
// synthetic shaders of gen_shaders.py (no emulator, no game data). docs/pipeline-library.md.
//
//   g++ -std=c++20 -O2 -o gpl_probe gpl_probe.cpp -lvulkan
//   gpl_probe DIR STEP...      (DIR holds vs.spv fs.spv fs2.spv; run.sh picks the steps)
//
// Each step prints "step <name> <ms>". The driver's disk cache is whatever the environment
// says (run.sh points __GL_SHADER_DISK_CACHE_PATH at a private directory).

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

namespace {

void Check(VkResult result, const char* what) {
    if (result != VK_SUCCESS) {
        std::fprintf(stderr, "%s failed: %d\n", what, static_cast<int>(result));
        std::exit(1);
    }
}

std::vector<uint32_t> ReadSpirv(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        std::fprintf(stderr, "cannot read %s\n", path.c_str());
        std::exit(1);
    }
    std::vector<uint32_t> words(static_cast<size_t>(file.tellg()) / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(words.data()), static_cast<std::streamsize>(words.size() * 4));
    return words;
}

double Ms(std::chrono::steady_clock::time_point begin) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin)
        .count();
}

struct State {
    VkBool32 blend = VK_FALSE;
    VkCullModeFlags cull = VK_CULL_MODE_NONE;
};

class Probe {
public:
    explicit Probe(const std::string& dir) {
        CreateDevice();
        vs = Module(dir + "/vs.spv");
        fs = Module(dir + "/fs.spv");
        fs2 = Module(dir + "/fs2.spv");
        rp_a = RenderPass(VK_FORMAT_R8G8B8A8_UNORM);
        rp_b = RenderPass(VK_FORMAT_R16G16B16A16_SFLOAT);
        layout = Layout(8);
        layout2 = Layout(9);
    }

    void Run(const std::string& step) {
        const auto begin = std::chrono::steady_clock::now();
        if (step == "mono") {
            Monolithic(fs, {}, rp_a);
        } else if (step == "mono_blend") {
            Monolithic(fs, {.blend = VK_TRUE}, rp_a);
        } else if (step == "mono_cull") {
            Monolithic(fs, {.cull = VK_CULL_MODE_BACK_BIT}, rp_a);
        } else if (step == "mono_rp_b") {
            Monolithic(fs, {}, rp_b);
        } else if (step == "mono_fs2") {
            Monolithic(fs2, {}, rp_a);
        } else if (step == "lib_vi") {
            libs["vi"] = VertexInput();
        } else if (step == "lib_pre") {
            libs["pre"] = PreRasterization(layout, rp_a, {});
        } else if (step == "lib_pre_cull") {
            libs["pre_cull"] = PreRasterization(layout, rp_a, {.cull = VK_CULL_MODE_BACK_BIT});
        } else if (step == "lib_pre_layout2") {
            libs["pre_layout2"] = PreRasterization(layout2, rp_a, {});
        } else if (step == "lib_pre_rp_b") {
            libs["pre_rp_b"] = PreRasterization(layout, rp_b, {});
        } else if (step == "lib_fs") {
            libs["fs"] = FragmentShader(fs, layout, rp_a);
        } else if (step == "lib_fs2") {
            libs["fs2"] = FragmentShader(fs2, layout, rp_a);
        } else if (step == "lib_fo") {
            libs["fo"] = FragmentOutput({}, rp_a);
        } else if (step == "lib_fo_blend") {
            libs["fo_blend"] = FragmentOutput({.blend = VK_TRUE}, rp_a);
        } else if (step == "link") {
            Link({"vi", "pre", "fs", "fo"}, layout, rp_a);
        } else if (step == "link_blend") {
            Link({"vi", "pre", "fs", "fo_blend"}, layout, rp_a);
        } else if (step == "link_fs2") {
            Link({"vi", "pre", "fs2", "fo"}, layout, rp_a);
        } else if (step == "link_cull") {
            Link({"vi", "pre_cull", "fs", "fo"}, layout, rp_a);
        } else if (step == "link_lto") {
            Link({"vi", "pre", "fs", "fo"}, layout, rp_a, true);
        } else {
            std::fprintf(stderr, "unknown step %s\n", step.c_str());
            std::exit(2);
        }
        std::printf("step %s %.3f\n", step.c_str(), Ms(begin));
        std::fflush(stdout);
    }

    ~Probe() {
        // Destroyed in order so that the driver flushes its disk cache as an application would.
        for (VkPipeline pipeline : created) {
            vkDestroyPipeline(device, pipeline, nullptr);
        }
        for (VkPipelineLayout pipeline_layout : {layout, layout2}) {
            vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
        }
        for (VkDescriptorSetLayout set : set_layouts) {
            vkDestroyDescriptorSetLayout(device, set, nullptr);
        }
        for (VkShaderModule module : {vs, fs, fs2}) {
            vkDestroyShaderModule(device, module, nullptr);
        }
        for (VkRenderPass pass : {rp_a, rp_b}) {
            vkDestroyRenderPass(device, pass, nullptr);
        }
        vkDestroyDevice(device, nullptr);
        vkDestroyInstance(instance, nullptr);
    }

    void PrintDevice() const {
        VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT gpl{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT};
        VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        driver.pNext = &gpl;
        VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        props.pNext = &driver;
        vkGetPhysicalDeviceProperties2(physical, &props);
        std::printf("device %s driver %s fast_linking=%u independent_interpolation=%u mesh=%u\n",
                    props.properties.deviceName, driver.driverInfo,
                    gpl.graphicsPipelineLibraryFastLinking,
                    gpl.graphicsPipelineLibraryIndependentInterpolationDecoration, mesh_supported);
    }

private:
    void CreateDevice() {
        const VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "gpl_probe", 1,
                                    nullptr, 0, VK_API_VERSION_1_3};
        const VkInstanceCreateInfo instance_ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, nullptr, 0,
                                               &app, 0, nullptr, 0, nullptr};
        Check(vkCreateInstance(&instance_ci, nullptr, &instance), "vkCreateInstance");
        uint32_t count = 0;
        vkEnumeratePhysicalDevices(instance, &count, nullptr);
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(instance, &count, devices.data());
        physical = VK_NULL_HANDLE;
        for (VkPhysicalDevice candidate : devices) {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(candidate, &props);
            if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                physical = candidate;
                break;
            }
        }
        if (physical == VK_NULL_HANDLE) {
            std::fprintf(stderr, "no discrete GPU\n");
            std::exit(1);
        }
        uint32_t ext_count = 0;
        vkEnumerateDeviceExtensionProperties(physical, nullptr, &ext_count, nullptr);
        std::vector<VkExtensionProperties> exts(ext_count);
        vkEnumerateDeviceExtensionProperties(physical, nullptr, &ext_count, exts.data());
        for (const auto& ext : exts) {
            mesh_supported |= std::strcmp(ext.extensionName, VK_EXT_MESH_SHADER_EXTENSION_NAME) == 0;
        }
        const float priority = 1.0f;
        const VkDeviceQueueCreateInfo queue_ci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr,
                                               0, 0, 1, &priority};
        VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gpl{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
        gpl.graphicsPipelineLibrary = VK_TRUE;
        const std::array<const char*, 2> names{VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME,
                                               VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME};
        const VkDeviceCreateInfo device_ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                           &gpl,
                                           0,
                                           1,
                                           &queue_ci,
                                           0,
                                           nullptr,
                                           static_cast<uint32_t>(names.size()),
                                           names.data(),
                                           nullptr};
        Check(vkCreateDevice(physical, &device_ci, nullptr, &device), "vkCreateDevice");
    }

    VkShaderModule Module(const std::string& path) {
        const std::vector<uint32_t> code = ReadSpirv(path);
        const VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0,
                                          code.size() * 4, code.data()};
        VkShaderModule module;
        Check(vkCreateShaderModule(device, &ci, nullptr, &module), "vkCreateShaderModule");
        return module;
    }

    VkRenderPass RenderPass(VkFormat color) {
        const std::array<VkAttachmentDescription, 2> attachments{{
            {0, color, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_LOAD,
             VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
             VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL},
            {0, VK_FORMAT_D32_SFLOAT, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_LOAD,
             VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
             VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL},
        }};
        const VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_GENERAL};
        const VkAttachmentReference depth_ref{1, VK_IMAGE_LAYOUT_GENERAL};
        const VkSubpassDescription subpass{0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, nullptr, 1,
                                           &color_ref, nullptr, &depth_ref, 0, nullptr};
        const VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, nullptr, 0, 2,
                                        attachments.data(), 1, &subpass, 0, nullptr};
        VkRenderPass pass;
        Check(vkCreateRenderPass(device, &ci, nullptr, &pass), "vkCreateRenderPass");
        return pass;
    }

    /// One set like the emulator's: vertex UBO + SSBO, then fragment samplers.
    VkPipelineLayout Layout(uint32_t samplers) {
        std::vector<VkDescriptorSetLayoutBinding> bindings{
            {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
        };
        for (uint32_t i = 0; i < samplers; ++i) {
            bindings.push_back({2 + i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
        }
        const VkDescriptorSetLayoutCreateInfo set_ci{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0,
            static_cast<uint32_t>(bindings.size()), bindings.data()};
        VkDescriptorSetLayout set;
        Check(vkCreateDescriptorSetLayout(device, &set_ci, nullptr, &set), "set layout");
        set_layouts.push_back(set);
        const VkPushConstantRange range{VK_SHADER_STAGE_ALL_GRAPHICS, 0, 64};
        const VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr,
                                            0, 1, &set, 1, &range};
        VkPipelineLayout result;
        Check(vkCreatePipelineLayout(device, &ci, nullptr, &result), "pipeline layout");
        return result;
    }

    // The fixed-function structures, as the emulator fills them (one RGBA target + depth).
    struct Fixed {
        VkVertexInputBindingDescription binding{0, 32, VK_VERTEX_INPUT_RATE_VERTEX};
        std::array<VkVertexInputAttributeDescription, 2> attributes{{
            {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
            {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 16},
        }};
        VkPipelineVertexInputStateCreateInfo vertex_input{
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, nullptr, 0, 1, &binding, 2,
            attributes.data()};
        VkPipelineInputAssemblyStateCreateInfo input_assembly{
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, nullptr, 0,
            VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE};
        VkPipelineViewportStateCreateInfo viewport{
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, nullptr, 0, 1, nullptr, 1,
            nullptr};
        VkPipelineRasterizationStateCreateInfo rasterization{
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            nullptr,
            0,
            VK_FALSE,
            VK_FALSE,
            VK_POLYGON_MODE_FILL,
            VK_CULL_MODE_NONE,
            VK_FRONT_FACE_COUNTER_CLOCKWISE,
            VK_FALSE,
            0.0f,
            0.0f,
            0.0f,
            1.0f};
        VkPipelineMultisampleStateCreateInfo multisample{
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            nullptr,
            0,
            VK_SAMPLE_COUNT_1_BIT,
            VK_FALSE,
            0.0f,
            nullptr,
            VK_FALSE,
            VK_FALSE};
        VkPipelineDepthStencilStateCreateInfo depth_stencil{
            VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
            nullptr,
            0,
            VK_TRUE,
            VK_TRUE,
            VK_COMPARE_OP_LESS,
            VK_FALSE,
            VK_FALSE,
            {},
            {},
            0.0f,
            1.0f};
        VkPipelineColorBlendAttachmentState attachment{VK_FALSE,
                                                       VK_BLEND_FACTOR_SRC_ALPHA,
                                                       VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                                       VK_BLEND_OP_ADD,
                                                       VK_BLEND_FACTOR_ONE,
                                                       VK_BLEND_FACTOR_ZERO,
                                                       VK_BLEND_OP_ADD,
                                                       0xF};
        VkPipelineColorBlendStateCreateInfo color_blend{
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            nullptr,
            0,
            VK_FALSE,
            VK_LOGIC_OP_COPY,
            1,
            &attachment,
            {}};
        std::array<VkDynamicState, 2> dynamic_states{VK_DYNAMIC_STATE_VIEWPORT,
                                                     VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                                 nullptr, 0, 2, dynamic_states.data()};

        explicit Fixed(const State& state) {
            rasterization.cullMode = state.cull;
            attachment.blendEnable = state.blend;
        }
    };

    VkPipelineShaderStageCreateInfo Stage(VkShaderStageFlagBits stage, VkShaderModule module) {
        return {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, stage, module,
                "main", nullptr};
    }

    VkPipeline Create(const VkGraphicsPipelineCreateInfo& ci, const char* what) {
        VkPipeline pipeline;
        Check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline), what);
        created.push_back(pipeline);
        return pipeline;
    }

    void Monolithic(VkShaderModule fragment, const State& state, VkRenderPass pass) {
        Fixed fixed{state};
        const std::array stages{Stage(VK_SHADER_STAGE_VERTEX_BIT, vs),
                                Stage(VK_SHADER_STAGE_FRAGMENT_BIT, fragment)};
        const VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                              nullptr,
                                              0,
                                              2,
                                              stages.data(),
                                              &fixed.vertex_input,
                                              &fixed.input_assembly,
                                              nullptr,
                                              &fixed.viewport,
                                              &fixed.rasterization,
                                              &fixed.multisample,
                                              &fixed.depth_stencil,
                                              &fixed.color_blend,
                                              &fixed.dynamic,
                                              layout,
                                              pass,
                                              0,
                                              VK_NULL_HANDLE,
                                              0};
        Create(ci, "monolithic");
    }

    VkGraphicsPipelineCreateInfo LibraryCi(VkGraphicsPipelineLibraryCreateInfoEXT& library,
                                           VkGraphicsPipelineLibraryFlagsEXT flags,
                                           const Fixed& fixed) {
        library = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT, nullptr, flags};
        VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        ci.pNext = &library;
        // The emulator does not retain link-time optimization info; GPL_PROBE_RETAIN=1 does,
        // for the link_lto step.
        ci.flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR |
                   (retain ? VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT : 0);
        ci.pDynamicState = &fixed.dynamic;
        return ci;
    }

    VkPipeline VertexInput() {
        Fixed fixed{{}};
        VkGraphicsPipelineLibraryCreateInfoEXT library;
        VkGraphicsPipelineCreateInfo ci = LibraryCi(
            library, VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT, fixed);
        ci.pVertexInputState = &fixed.vertex_input;
        ci.pInputAssemblyState = &fixed.input_assembly;
        return Create(ci, "vertex input library");
    }

    VkPipeline PreRasterization(VkPipelineLayout pipeline_layout, VkRenderPass pass,
                                const State& state) {
        Fixed fixed{state};
        VkGraphicsPipelineLibraryCreateInfoEXT library;
        VkGraphicsPipelineCreateInfo ci = LibraryCi(
            library, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT, fixed);
        const VkPipelineShaderStageCreateInfo stage = Stage(VK_SHADER_STAGE_VERTEX_BIT, vs);
        ci.stageCount = 1;
        ci.pStages = &stage;
        ci.pViewportState = &fixed.viewport;
        ci.pRasterizationState = &fixed.rasterization;
        ci.layout = pipeline_layout;
        ci.renderPass = pass;
        return Create(ci, "pre-rasterization library");
    }

    VkPipeline FragmentShader(VkShaderModule module, VkPipelineLayout pipeline_layout,
                              VkRenderPass pass) {
        Fixed fixed{{}};
        VkGraphicsPipelineLibraryCreateInfoEXT library;
        VkGraphicsPipelineCreateInfo ci =
            LibraryCi(library, VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT, fixed);
        const VkPipelineShaderStageCreateInfo stage = Stage(VK_SHADER_STAGE_FRAGMENT_BIT, module);
        ci.stageCount = 1;
        ci.pStages = &stage;
        ci.pMultisampleState = &fixed.multisample;
        ci.pDepthStencilState = &fixed.depth_stencil;
        ci.layout = pipeline_layout;
        ci.renderPass = pass;
        return Create(ci, "fragment shader library");
    }

    VkPipeline FragmentOutput(const State& state, VkRenderPass pass) {
        Fixed fixed{state};
        VkGraphicsPipelineLibraryCreateInfoEXT library;
        VkGraphicsPipelineCreateInfo ci = LibraryCi(
            library, VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT, fixed);
        ci.pMultisampleState = &fixed.multisample;
        ci.pColorBlendState = &fixed.color_blend;
        ci.renderPass = pass;
        return Create(ci, "fragment output library");
    }

    void Link(const std::vector<std::string>& names, VkPipelineLayout pipeline_layout,
              VkRenderPass pass, bool lto = false) {
        if (lto && !retain) {
            std::fprintf(stderr, "link_lto needs GPL_PROBE_RETAIN=1\n");
            std::exit(2);
        }
        std::vector<VkPipeline> parts;
        for (const std::string& name : names) {
            const auto it = libs.find(name);
            if (it == libs.end()) {
                std::fprintf(stderr, "link needs step lib_%s first\n", name.c_str());
                std::exit(2);
            }
            parts.push_back(it->second);
        }
        const VkPipelineLibraryCreateInfoKHR library{VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR,
                                                     nullptr, static_cast<uint32_t>(parts.size()),
                                                     parts.data()};
        VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        ci.pNext = &library;
        ci.flags = lto ? VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT : 0;
        ci.layout = pipeline_layout;
        ci.renderPass = pass;
        Create(ci, "link");
    }

    const bool retain = [] {
        const char* value = std::getenv("GPL_PROBE_RETAIN");
        return value != nullptr && value[0] == '1';
    }();
    std::vector<VkPipeline> created;
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkDevice device{};
    bool mesh_supported{};
    VkShaderModule vs{}, fs{}, fs2{};
    VkRenderPass rp_a{}, rp_b{};
    VkPipelineLayout layout{}, layout2{};
    std::vector<VkDescriptorSetLayout> set_layouts;
    std::map<std::string, VkPipeline> libs;
};

} // Anonymous namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s DIR STEP...\n", argv[0]);
        return 2;
    }
    Probe probe{argv[1]};
    probe.PrintDevice();
    for (int i = 2; i < argc; ++i) {
        probe.Run(argv[i]);
    }
    return 0;
}
