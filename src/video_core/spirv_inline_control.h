// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "common/common_types.h"

namespace VideoCommon {

/// Marks DontInline (and clears Inline on) every function of a SPIR-V module that is not an entry
/// point, has at least `min_instructions` instructions and is called from at least `min_calls`
/// places. Returns how many functions it marked; a malformed module is left as it is (0).
///
/// The global memory functions of a native program (spirv_emit_context.cpp,
/// DefineGlobalMemoryFunctions) test the address against every storage buffer, several hundred
/// instructions each, and a compute shader calls them from dozens of places. NVIDIA's compiler
/// inlines every call: Ocarina's compute shaders took 2-13 s to build (RTX 3080 Ti, 615.78) and
/// froze the emulator at their first dispatch. Called, they build in a fraction of that.
std::size_t MarkSharedFunctionsDontInline(std::span<u32> words, std::size_t min_instructions = 128,
                                          std::size_t min_calls = 2);

/// Shader stage of a SPIR-V module, from the execution model of its entry points.
enum class SpirvStage : u32 {
    Vertex,
    TessControl,
    TessEval,
    Geometry,
    Fragment,
    Compute,
    Task,
    Mesh,
    Unknown, ///< No entry point, entry points of different stages, or a malformed module.
};

/// The stage of a module's entry points (Unknown unless they all share one).
[[nodiscard]] SpirvStage ModuleStage(std::span<const u32> words);

/// Bit of a stage in a graphics no-inline stage mask.
[[nodiscard]] constexpr u32 StageBit(SpirvStage stage) {
    return 1u << static_cast<u32>(stage);
}

/// Stages EDEN_NX2_GRAPHICS_NOINLINE applies to by default: every graphics stage but fragment.
/// A call costs once per vertex, patch or primitive there; a fragment shader runs per pixel, so
/// it keeps the driver's inlining unless asked.
inline constexpr u32 DefaultGraphicsNoInlineStages =
    StageBit(SpirvStage::Vertex) | StageBit(SpirvStage::TessControl) |
    StageBit(SpirvStage::TessEval) | StageBit(SpirvStage::Geometry) |
    StageBit(SpirvStage::Task) | StageBit(SpirvStage::Mesh);
inline constexpr u32 AllGraphicsNoInlineStages =
    DefaultGraphicsNoInlineStages | StageBit(SpirvStage::Fragment);

/// Stage mask for an EDEN_NX2_GRAPHICS_NOINLINE value (null when unset): unset, empty or "1" is
/// the default; "0" or "none" none; "all" every graphics stage; otherwise a set of letters,
/// v vertex, c tess control, e tess eval, g geometry, t task, m mesh, f fragment ("vtm"). A value
/// with any other character is the default. Compute is never in the mask (it has its own switch).
[[nodiscard]] u32 ParseGraphicsNoInlineStages(const char* value);

/// The process's EDEN_NX2_GRAPHICS_NOINLINE mask, read once.
[[nodiscard]] u32 GraphicsNoInlineStages();

/// The words a graphics module is built from: when the module's stage is in `stage_mask`,
/// MarkSharedFunctionsDontInline over a copy of `code`, stored in `storage`. Returns `code`
/// itself when nothing changes (other stages, compute, nothing marked); `marked` gets the count.
[[nodiscard]] std::span<const u32> GraphicsModuleCode(std::span<const u32> code, u32 stage_mask,
                                                      std::vector<u32>& storage,
                                                      std::size_t& marked);

} // namespace VideoCommon
