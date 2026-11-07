// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdlib>
#include <vector>

#include "common/common_types.h"
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Backend::SPIRV {

constexpr u32 NUM_TEXTURE_SCALING_WORDS = 4;
constexpr u32 NUM_IMAGE_SCALING_WORDS = 2;
constexpr u32 NUM_TEXTURE_AND_IMAGE_SCALING_WORDS =
    NUM_TEXTURE_SCALING_WORDS + NUM_IMAGE_SCALING_WORDS;

struct RescalingLayout {
    alignas(16) std::array<u32, NUM_TEXTURE_SCALING_WORDS> rescaling_textures;
    alignas(16) std::array<u32, NUM_IMAGE_SCALING_WORDS> rescaling_images;
    u32 down_factor;
};
struct RenderAreaLayout {
    std::array<f32, 4> render_area;
};
constexpr u32 RESCALING_LAYOUT_WORDS_OFFSET = offsetof(RescalingLayout, rescaling_textures);
constexpr u32 RESCALING_LAYOUT_DOWN_FACTOR_OFFSET = offsetof(RescalingLayout, down_factor);
constexpr u32 RENDERAREA_LAYOUT_OFFSET = offsetof(RenderAreaLayout, render_area);

// EDEN_NX2_MESH_FIRST=1: the draw's first vertex is the first task (or mesh, without a task)
// workgroup index, as in NVN and GL_NV_mesh_shader. Only the first active stage reads non-zero.
struct MeshDrawBaseLayout {
    u32 task;
    u32 mesh;
};
// Base addresses (u64) of a native program's native_indirect storage buffers, after the
// rescaling and render area words: a compute program's 8, or a draw's task program's 4 followed by
// its mesh program's 4 (Info::MaxNativeIndirectBindings).
constexpr u32 NATIVE_INDIRECT_BASES_OFFSET = 64;
constexpr u32 NATIVE_INDIRECT_BASES_SIZE = 8 * sizeof(u64);
static_assert(NATIVE_INDIRECT_BASES_OFFSET >= sizeof(RescalingLayout) + sizeof(RenderAreaLayout));
// Within the 128 bytes of push constants every Vulkan device supports
static_assert(NATIVE_INDIRECT_BASES_OFFSET + NATIVE_INDIRECT_BASES_SIZE <= 128);
static_assert(2 * Info::MAX_NATIVE_INDIRECT_GRAPHICS_BINDINGS * sizeof(u64) <=
              NATIVE_INDIRECT_BASES_SIZE);
/// The first of a stage's bases in the NATIVE_INDIRECT_BASES_SIZE bytes, in u64 units
constexpr u32 NativeIndirectBasesFirst(Stage stage) {
    return stage == Stage::Mesh ? static_cast<u32>(Info::MAX_NATIVE_INDIRECT_GRAPHICS_BINDINGS)
                                : 0U;
}

constexpr u32 MESH_DRAW_BASE_OFFSET = sizeof(RescalingLayout);
static_assert(MESH_DRAW_BASE_OFFSET + sizeof(MeshDrawBaseLayout) <=
              sizeof(RescalingLayout) + sizeof(RenderAreaLayout));

inline bool MeshFirstEnabled() {
    static const bool enabled{std::getenv("EDEN_NX2_MESH_FIRST") != nullptr};
    return enabled;
}

[[nodiscard]] std::vector<u32> EmitSPIRV(const Profile& profile, const RuntimeInfo& runtime_info, IR::Program& program, Bindings& bindings);
[[nodiscard]] inline std::vector<u32> EmitSPIRV(const Profile& profile, IR::Program& program) {
    Bindings binding;
    return EmitSPIRV(profile, {}, program, binding);
}

/// Words per lane in the buffer of EmitCooperativeHmmaTest: the guest registers a0 a1 a2 a3 b0 b1
/// c0 c1 as input, then d0 d1 as output.
constexpr u32 COOPERATIVE_HMMA_TEST_LANE_WORDS = 10;

/// A compute shader of one warp running the HMMA.16816.F16 lowering on the registers in its storage
/// buffer (set 0, binding 0), to check at startup that the driver's matrix layout is the expected.
[[nodiscard]] std::vector<u32> EmitCooperativeHmmaTest(u32 spirv_version);

} // namespace Shader::Backend::SPIRV
