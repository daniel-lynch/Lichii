// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ir/program.h"

namespace Shader {
struct HostTranslateInfo;
}

namespace Shader::Optimization {

void CollectShaderInfoPass(Environment& env, IR::Program& program);
void ConditionalBarrierPass(IR::Program& program);
void ConstantPropagationPass(Environment& env, IR::Program& program);
void DeadCodeEliminationPass(IR::Program& program);
void GeometryCompactionPass(IR::Program& program, const HostTranslateInfo& host_info);
void GlobalMemoryToStorageBufferPass(Environment& env, IR::Program& program,
                                     const HostTranslateInfo& host_info);
void IdentityRemovalPass(IR::Program& program);
void LowerFp64ToFp32(IR::Program& program);
void LowerFp16ToFp32(IR::Program& program);
void LowerInt64ToInt32(IR::Program& program);
void RescalingPass(IR::Program& program);
void SsaRewritePass(IR::Program& program);
void PositionPass(Environment& env, IR::Program& program);
void BlitConstantsPass(Environment& env, IR::Program& program);
void TexturePass(Environment& env, IR::Program& program, const HostTranslateInfo& host_info);
void LimitSampledTextureArrays(IR::Program& program, const HostTranslateInfo& host_info);
void LayerPass(IR::Program& program, const HostTranslateInfo& host_info);
void VendorWorkaroundPass(IR::Program& program);
void VerificationPass(const IR::Program& program);
bool HasBrokenPattern(const IR::Program& program);
/// EDEN_NX2_DYN_CBUF_POINTERS=1 (off by default since upstream #150, read once): native global
/// accesses whose base pointer is read from a constant buffer at a dynamic offset bind every
/// pointer of that buffer, instead of upstream's slot walk.
bool NativeDynamicCbufPointers();
/// EDEN_NX2_INDIRECT_POINTERS=1 or =guest (off by default since upstream #150): native loads
/// through a pointer loaded from a record that upstream's table entries leave unbound get
/// read-only native_indirect bindings, and the runtime pick checks the bound length.
bool NativeIndirectPointers();
/// The SPIR-V runtime pick of native global accesses takes only a buffer whose bound length
/// (OpArrayLength) reaches the address: a miss loads zero and stores nothing instead of landing
/// in the closest buffer below it. On by default (EDEN_NX2_SSBO_BOUNDS_CHECK=0 turns it off
/// unless EDEN_NX2_INDIRECT_POINTERS is set). Read once.
bool NativeStorageBoundsCheck();
/// Also for task and mesh programs (bound per draw); EDEN_NX2_INDIRECT_POINTERS=compute: compute
/// programs only
bool NativeIndirectGraphicsPointers();

// Dual Vertex
void VertexATransformPass(IR::Program& program);
void VertexBTransformPass(IR::Program& program);
void JoinTextureInfo(Info& base, Info& source);
void JoinStorageInfo(Info& base, Info& source);

} // namespace Shader::Optimization
