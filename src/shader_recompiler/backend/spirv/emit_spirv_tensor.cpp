// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <span>
#include <vector>

#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "common/common_types.h"
#include "shader_recompiler/frontend/ir/program.h"

namespace Shader::Backend::SPIRV {
namespace {
// SPV_KHR_cooperative_matrix enums (spirv.hpp11 names vary between header versions).
constexpr u32 ScopeSubgroup = 3;
constexpr u32 UseMatrixA = 0;
constexpr u32 UseMatrixB = 1;
constexpr u32 UseAccumulator = 2;
constexpr u32 LayoutRowMajor = 0;
constexpr u32 LayoutColumnMajor = 1;
// Subgroup scope, acquire-release on workgroup memory.
constexpr u32 SubgroupBarrierSemantics = 0x8 | 0x100;

void DefineCooperativeMatrixTypes(EmitContext& ctx) {
    ctx.AddExtension("SPV_KHR_cooperative_matrix");
    ctx.AddCapability(spv::Capability::CooperativeMatrixKHR);
    ctx.AddCapability(spv::Capability::Float16);
    ctx.AddCapability(spv::Capability::GroupNonUniform);
    const Id f16 = ctx.TypeFloat(16);
    const Id scope = ctx.Const(ScopeSubgroup);
    const Id sixteen = ctx.Const(16U);
    // OpTypeCooperativeMatrixKHR %f16 %scope %rows %columns %use, marked as an OpTypeStruct with
    // the same operands (constants, so it can't collide with a real struct).
    const auto matrix_type = [&](u32 use) {
        const Id type = ctx.TypeStruct(f16, scope, sixteen, sixteen, ctx.Const(use));
        ctx.coopmat_type_markers.push_back(type.value);
        return type;
    };
    ctx.tensor_type_a = matrix_type(UseMatrixA);
    ctx.tensor_type_b = matrix_type(UseMatrixB);
    ctx.tensor_type_acc = matrix_type(UseAccumulator);
    const Id input_u32 = ctx.TypePointer(spv::StorageClass::Input, ctx.U32[1]);
    ctx.tensor_subgroup_id = ctx.AddGlobalVariable(input_u32, spv::StorageClass::Input);
    ctx.Decorate(ctx.tensor_subgroup_id, spv::Decoration::BuiltIn, spv::BuiltIn::SubgroupId);
    ctx.interfaces.push_back(ctx.tensor_subgroup_id);
}

Id SharedWord(EmitContext& ctx, Id index) {
    if (ctx.uses_explicit_workgroup_layout) {
        return ctx.OpAccessChain(ctx.shared_u32, ctx.shared_memory_u32, ctx.u32_zero_value, index);
    }
    return ctx.OpAccessChain(ctx.shared_u32, ctx.shared_memory_u32, index);
}

// OpCooperativeMatrixLoadKHR / MulAddKHR: result type, result and three ids, like OpSelect.
Id CooperativeMatrixOp(EmitContext& ctx, spv::Op op, Id result_type, Id op1, Id op2, Id op3) {
    const Id result = ctx.OpSelect(result_type, op1, op2, op3);
    ctx.coopmat_op_markers.emplace_back(result.value, op);
    return result;
}

// OpCooperativeMatrixStoreKHR %pointer %object %layout %stride: an OpStore with two extra words.
void CooperativeMatrixStore(EmitContext& ctx, Id pointer, Id object, Id layout, Id stride) {
    const std::array<Sirit::MemoryAccessLiteral, 2> extra{layout.value, stride.value};
    ctx.OpStore(pointer, object, std::span<const Sirit::MemoryAccessLiteral>(extra));
    ctx.coopmat_store_markers.push_back(pointer.value);
}

void SubgroupBarrier(EmitContext& ctx) {
    ctx.OpControlBarrier(ctx.Const(ScopeSubgroup), ctx.Const(ScopeSubgroup),
                         ctx.Const(SubgroupBarrierSemantics));
}

// One HMMA.16816.F16 (or .1688 with zero upper halves) of a 32-lane warp: the NVIDIA fragments
// go to row-major A, column-major B (columns 8-15 zero) and row-major C tiles in shared memory,
// one 16x16x16 cooperative-matrix multiply-add replaces the per-lane emulation, and each lane
// reads its two D words back from where it wrote C. For depth 8 the upper K halves are stored as
// zero here, as the software helper ignores them, instead of trusting the caller to pass zeros.
Id EmitCooperativeTensorMulAdd(EmitContext& ctx, Id a, Id b, Id c, u32 depth) {
    const Id u32ty = ctx.U32[1];
    const auto imm = [&](u32 value) { return ctx.Const(value); };
    const auto add = [&](Id lhs, Id rhs) { return ctx.OpIAdd(u32ty, lhs, rhs); };
    const Id lane = ctx.OpBitwiseAnd(u32ty, ctx.OpLoad(u32ty, ctx.subgroup_local_invocation_id),
                                     imm(31));
    const Id subgroup = ctx.OpLoad(u32ty, ctx.tensor_subgroup_id);
    const Id base = add(imm(ctx.tensor_scratch_word), ctx.OpIMul(u32ty, subgroup, imm(384)));
    const Id tile_a = base;
    const Id tile_b = add(base, imm(128));
    const Id tile_c = add(base, imm(256));
    const Id g = ctx.OpShiftRightLogical(u32ty, lane, imm(2));
    const Id t = ctx.OpBitwiseAnd(u32ty, lane, imm(3));
    // Word of (row, first column pair) in a 16x16 fp16 tile with 8 words a row/column.
    const Id row_lo = add(ctx.OpShiftLeftLogical(u32ty, g, imm(3)), t);  // row g,   cols 2t
    const Id row_hi = add(row_lo, imm(64));                                // row g+8, cols 2t
    const auto store = [&](Id tile, Id word, Id value) {
        ctx.OpStore(SharedWord(ctx, add(tile, word)), value);
    };
    store(tile_a, row_lo, ctx.OpCompositeExtract(u32ty, a, 0));
    store(tile_a, row_hi, ctx.OpCompositeExtract(u32ty, a, 1));
    const bool short_depth = depth == 8;
    store(tile_a, add(row_lo, imm(4)),
          short_depth ? ctx.u32_zero_value : ctx.OpCompositeExtract(u32ty, a, 2));
    store(tile_a, add(row_hi, imm(4)),
          short_depth ? ctx.u32_zero_value : ctx.OpCompositeExtract(u32ty, a, 3));
    // B column g holds k pairs t (b0) and t+4 (b1); columns 8-15 (words 64-127) are zero.
    store(tile_b, row_lo, ctx.OpCompositeExtract(u32ty, b, 0));
    store(tile_b, add(row_lo, imm(4)),
          short_depth ? ctx.u32_zero_value : ctx.OpCompositeExtract(u32ty, b, 1));
    store(tile_b, add(lane, imm(64)), ctx.u32_zero_value);
    store(tile_b, add(lane, imm(96)), ctx.u32_zero_value);
    store(tile_c, row_lo, ctx.OpCompositeExtract(u32ty, c, 0));
    store(tile_c, row_hi, ctx.OpCompositeExtract(u32ty, c, 1));
    SubgroupBarrier(ctx);
    const Id stride = imm(8);
    const auto load = [&](Id type, Id tile, u32 layout) {
        return CooperativeMatrixOp(ctx, spv::Op::OpCooperativeMatrixLoadKHR, type,
                                   SharedWord(ctx, tile), imm(layout), stride);
    };
    const Id matrix_a = load(ctx.tensor_type_a, tile_a, LayoutRowMajor);
    const Id matrix_b = load(ctx.tensor_type_b, tile_b, LayoutColumnMajor);
    const Id matrix_c = load(ctx.tensor_type_acc, tile_c, LayoutRowMajor);
    const Id matrix_d = CooperativeMatrixOp(ctx, spv::Op::OpCooperativeMatrixMulAddKHR,
                                            ctx.tensor_type_acc, matrix_a, matrix_b, matrix_c);
    CooperativeMatrixStore(ctx, SharedWord(ctx, tile_c), matrix_d, imm(LayoutRowMajor), stride);
    SubgroupBarrier(ctx);
    const Id d0 = ctx.OpLoad(u32ty, SharedWord(ctx, add(tile_c, row_lo)));
    const Id d1 = ctx.OpLoad(u32ty, SharedWord(ctx, add(tile_c, row_hi)));
    return ctx.OpCompositeConstruct(ctx.U32[2], d0, d1);
}
} // Anonymous namespace

void DefineSoftwareMatrixFunctions(EmitContext& ctx, const IR::Program& program) {
    if (ctx.tensor_coopmat) {
        DefineCooperativeMatrixTypes(ctx);
        return;
    }
    std::array<bool, 2> needed{};
    for (const auto* block : program.blocks) {
        for (const auto& inst : block->Instructions()) {
            if (inst.GetOpcode() != IR::Opcode::TensorMulAdd || inst.Flags<u32>() == 0) continue;
            const u32 depth = inst.Flags<u32>();
            if (depth != 8 && depth != 16) throw LogicError("Invalid software HMMA depth");
            needed[depth == 16] = true;
        }
    }
    for (u32 variant = 0; variant < 2; ++variant) {
        if (!needed[variant]) continue;
        ctx.AddCapability(spv::Capability::GroupNonUniform);
        ctx.AddCapability(spv::Capability::GroupNonUniformShuffle);
        if (!ctx.profile.SupportsSubgroupStage(program.stage) ||
            ctx.profile.has_broken_spirv_subgroup_shuffle)
            throw NotImplementedException("Software HMMA requires subgroup shuffle support");
        const Id u32ty = ctx.U32[1], f32ty = ctx.F32[1];
        const auto imm = [&](u32 v) { return ctx.Const(v); };
        const auto add = [&](Id a, Id b) { return ctx.OpIAdd(u32ty, a, b); };
        const auto and_ = [&](Id a, u32 b) { return ctx.OpBitwiseAnd(u32ty, a, imm(b)); };
        const auto unpack = [&](Id v) { return ctx.OpUnpackHalf2x16(ctx.F32[2], v); };
        const auto round_half = [&](Id value) {
            // Do not express the intermediate rounding as unpack(pack(value)): drivers may
            // eliminate that round trip. Round normal significands explicitly, ties to even;
            // half subnormals lie on the exactly representable 2^-24 grid.
            const Id bits = ctx.OpBitcast(u32ty, value);
            const Id magnitude = and_(bits, 0x7fffffffU);
            const Id tie = and_(ctx.OpShiftRightLogical(u32ty, magnitude, imm(13)), 1);
            const Id rounded = and_(add(add(magnitude, imm(0xfff)), tie), ~0x1fffU);
            const Id normal = ctx.OpSelect(u32ty,
                ctx.OpUGreaterThanEqual(ctx.U1, rounded, imm(0x47800000)), imm(0x7f800000), rounded);
            const Id scaled = ctx.OpFMul(f32ty, ctx.OpBitcast(f32ty, magnitude), ctx.Const(16777216.0f));
            const Id subnormal = ctx.OpBitcast(u32ty,
                ctx.OpFMul(f32ty, ctx.OpRoundEven(f32ty, scaled), ctx.Const(0x1p-24f)));
            const Id finite = ctx.OpSelect(u32ty,
                ctx.OpUGreaterThanEqual(ctx.U1, magnitude, imm(0x38800000)), normal, subnormal);
            const Id result = ctx.OpSelect(u32ty,
                ctx.OpUGreaterThanEqual(ctx.U1, magnitude, imm(0x7f800000)), magnitude, finite);
            return ctx.OpBitcast(f32ty, ctx.OpBitwiseOr(u32ty, result, and_(bits, 0x80000000)));
        };
        const Id type = ctx.TypeFunction(ctx.U32[2], ctx.U32[4], ctx.U32[2], ctx.U32[2]);
        const Id fn = ctx.OpFunction(ctx.U32[2], spv::FunctionControlMask::DontInline, type);
        ctx.software_matrix_functions[variant] = fn;
        const Id matrix_a = ctx.OpFunctionParameter(ctx.U32[4]);
        const Id matrix_b = ctx.OpFunctionParameter(ctx.U32[2]);
        const Id matrix_c = ctx.OpFunctionParameter(ctx.U32[2]);
        ctx.AddLabel();
        const Id counter = ctx.AddLocalVariable(ctx.TypePointer(spv::StorageClass::Function, u32ty),
                                           spv::StorageClass::Function);
        std::array<Id, 4> accumulators;
        for (auto& acc : accumulators)
            acc = ctx.AddLocalVariable(ctx.TypePointer(spv::StorageClass::Function, f32ty),
                                 spv::StorageClass::Function);
        std::array<Id, 8> inputs;
        for (u32 i = 0; i < 4; ++i)
            inputs[i] = ctx.OpCompositeExtract(u32ty, matrix_a, i);
        for (u32 i = 0; i < 2; ++i) {
            inputs[4 + i] = ctx.OpCompositeExtract(u32ty, matrix_b, i);
            inputs[6 + i] = ctx.OpCompositeExtract(u32ty, matrix_c, i);
        }
        for (u32 row = 0; row < 2; ++row) {
            const Id c = unpack(inputs[6 + row]);
            for (u32 col = 0; col < 2; ++col)
                ctx.OpStore(accumulators[row * 2 + col], ctx.OpCompositeExtract(f32ty, c, col));
        }
        ctx.OpStore(counter, imm(0));
        const Id lane = ctx.OpLoad(u32ty, ctx.subgroup_local_invocation_id);
        const Id warp_base = and_(lane, ~31U);
        const Id row_base = and_(lane, 28);
        const Id col_base = ctx.OpShiftLeftLogical(u32ty, and_(lane, 3), imm(3));
        const Id header = ctx.OpLabel(), body = ctx.OpLabel(), next = ctx.OpLabel(), end = ctx.OpLabel();
        ctx.OpBranch(header);
        ctx.AddLabel(header);
        const Id pair = ctx.OpLoad(u32ty, counter);
        const Id more = ctx.OpULessThan(ctx.U1, pair, imm(variant ? 8 : 4));
        ctx.OpLoopMerge(end, next, spv::LoopControlMask::DontUnroll);
        ctx.OpBranchConditional(more, body, end);
        ctx.AddLabel(body);
        const Id upper = ctx.OpUGreaterThanEqual(ctx.U1, pair, imm(4));
        const Id component = and_(pair, 3);
        const Id a_lane = add(warp_base, add(row_base, component));
        const Id b_lane = add(warp_base, add(col_base, component));
        const Id b = ctx.OpSelect(u32ty, upper, inputs[5], inputs[4]);
        std::array<Id, 2> bs;
        for (u32 col = 0; col < 2; ++col)
            bs[col] = unpack(ctx.OpGroupNonUniformShuffle(u32ty, imm(3), b,
                                                          add(b_lane, imm(col * 4))));
        const Id round_now = ctx.OpIEqual(ctx.U1, component, imm(3));
        for (u32 row = 0; row < 2; ++row) {
            const Id a = ctx.OpSelect(u32ty, upper, inputs[row + 2], inputs[row]);
            const Id as = unpack(ctx.OpGroupNonUniformShuffle(u32ty, imm(3), a, a_lane));
            for (u32 col = 0; col < 2; ++col) {
                Id value = ctx.OpLoad(f32ty, accumulators[row * 2 + col]);
                for (u32 half = 0; half < 2; ++half) {
                    value = ctx.OpFma(f32ty, ctx.OpCompositeExtract(f32ty, as, half),
                                      ctx.OpCompositeExtract(f32ty, bs[col], half), value);
                    ctx.Decorate(value, spv::Decoration::NoContraction);
                }
                ctx.OpStore(accumulators[row * 2 + col], value);
            }
        }
        // HMMA.F16 rounds after each K=8 group, including the midpoint of K=16.
        // OpSelect evaluates both operands: keep the expensive half conversion in
        // a branch so the other three pairs do no rounding work. The counter is
        // identical in every participating lane and all shuffles precede this branch.
        const Id round_block = ctx.OpLabel(), round_merge = ctx.OpLabel();
        ctx.OpSelectionMerge(round_merge, spv::SelectionControlMask::DontFlatten);
        ctx.OpBranchConditional(round_now, round_block, round_merge);
        ctx.AddLabel(round_block);
        for (const Id accumulator : accumulators) {
            ctx.OpStore(accumulator, round_half(ctx.OpLoad(f32ty, accumulator)));
        }
        ctx.OpBranch(round_merge);
        ctx.AddLabel(round_merge);
        ctx.OpBranch(next);
        ctx.AddLabel(next);
        ctx.OpStore(counter, add(pair, imm(1)));
        ctx.OpBranch(header);
        ctx.AddLabel(end);
        std::array<Id, 2> packed;
        for (u32 row = 0; row < 2; ++row)
            packed[row] = ctx.OpPackHalf2x16(u32ty, ctx.OpCompositeConstruct(ctx.F32[2],
                ctx.OpLoad(f32ty, accumulators[row * 2]), ctx.OpLoad(f32ty, accumulators[row * 2 + 1])));
        ctx.OpReturnValue(ctx.OpCompositeConstruct(ctx.U32[2], packed[0], packed[1]));
        ctx.OpFunctionEnd();
    }
}

void PatchCooperativeMatrixInstructions(std::vector<u32>& code, const EmitContext& ctx) {
    if (ctx.coopmat_type_markers.empty()) {
        return;
    }
    const auto set_opcode = [&](size_t offset, spv::Op op) {
        code[offset] = (code[offset] & 0xFFFF0000u) | static_cast<u32>(op);
    };
    size_t offset = 5;
    while (offset < code.size()) {
        const u32 word_count{code[offset] >> 16};
        if (word_count == 0 || offset + word_count > code.size()) {
            break;
        }
        const auto opcode{static_cast<spv::Op>(code[offset] & 0xFFFFu)};
        if (opcode == spv::Op::OpTypeStruct && word_count == 7 &&
            std::ranges::find(ctx.coopmat_type_markers, code[offset + 1]) !=
                ctx.coopmat_type_markers.end()) {
            set_opcode(offset, spv::Op::OpTypeCooperativeMatrixKHR);
        } else if (opcode == spv::Op::OpSelect && word_count == 6) {
            const auto it{std::ranges::find(ctx.coopmat_op_markers, code[offset + 2],
                                            &std::pair<u32, spv::Op>::first)};
            if (it != ctx.coopmat_op_markers.end()) {
                set_opcode(offset, it->second);
            }
        } else if (opcode == spv::Op::OpStore && word_count == 5 &&
                   std::ranges::find(ctx.coopmat_store_markers, code[offset + 1]) !=
                       ctx.coopmat_store_markers.end()) {
            set_opcode(offset, spv::Op::OpCooperativeMatrixStoreKHR);
        }
        offset += word_count;
    }
}

Id EmitTensorMulAdd(EmitContext& ctx, IR::Inst* inst, Id a, Id b, Id c) {
    const u32 depth = inst->Flags<u32>();
    if (depth != 8 && depth != 16) {
        throw NotImplementedException("Unsupported matrix multiply depth");
    }
    if (ctx.tensor_coopmat) {
        return EmitCooperativeTensorMulAdd(ctx, a, b, c, depth);
    }
    return ctx.OpFunctionCall(ctx.U32[2], ctx.software_matrix_functions[depth == 16], a, b, c);
}

} // namespace Shader::Backend::SPIRV
