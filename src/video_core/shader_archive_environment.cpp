// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <limits>
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ampere/program_address_map.h"
#include "shader_recompiler/frontend/ampere/program_capture.h"
#include "shader_recompiler/frontend/ampere/program_override.h"
#include "video_core/shader_archive_environment.h"

namespace VideoCommon {
namespace {
u32 Word(std::span<const u8> bytes, size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(u32)) {
        throw Shader::RuntimeError("Archive environment: invalid_range");
    }
    return u32{bytes[offset]} | (u32{bytes[offset + 1]} << 8) |
           (u32{bytes[offset + 2]} << 16) | (u32{bytes[offset + 3]} << 24);
}
} // namespace

ArchiveEnvironment::ArchiveEnvironment(std::span<const u8> program, std::span<const u8> control,
                                       Shader::Stage shader_stage, u32 texture_buffer,
                                       Shader::TextureType texture_type_) : texture_type{texture_type_} {
    using namespace Shader::Ampere;
    tracks_texture_handles = true;
    if (shader_stage != Shader::Stage::VertexB && shader_stage != Shader::Stage::Fragment &&
        shader_stage != Shader::Stage::Geometry) {
        throw Shader::RuntimeError("Archive environment: unsupported_stage");
    }
    if (control.size() < 0x58 || Word(control, 0) != 0x98761234 ||
        Word(control, 4) != 0xc9 || Word(control, 8) != 20) {
        throw Shader::RuntimeError("Archive environment: unsupported_control");
    }
    if ((shader_stage == Shader::Stage::VertexB && control[0x56] != 0) ||
        (shader_stage == Shader::Stage::Fragment && control[0x56] != 1)) {
        throw Shader::RuntimeError("Archive environment: stage_mismatch");
    }
    constexpr size_t HeaderSize = 128;
    const u32 code_size = Word(control, 0x38);
    const u32 constant_size = Word(control, 0x3c);
    const u32 constant_offset = Word(control, 0x40);
    if (code_size < HeaderSize + sizeof(Instruction) || code_size > program.size() ||
        code_size % sizeof(Instruction) != 0 || constant_size > 0x100 ||
        (constant_size && (constant_offset < code_size || constant_offset > program.size() ||
                          constant_size > program.size() - constant_offset))) {
        throw Shader::RuntimeError("Archive environment: invalid_range");
    }
    stage = shader_stage;
    is_proprietary_driver = true;
    texture_bound = texture_buffer;
    initial_offset = sizeof(sph);
    static_assert(sizeof(sph) <= HeaderSize);
    std::memcpy(&sph, program.data(), sizeof(sph));
    const u32 header_stage = shader_stage == Shader::Stage::VertexB ? 1 :
                             shader_stage == Shader::Stage::Fragment ? 5 : 4;
    if (sph.common0.shader_type != header_stage) {
        throw Shader::RuntimeError("Archive environment: stage_mismatch");
    }
    const u64 local_size = sph.LocalMemorySize() + sph.common3.shader_local_memory_crs_size;
    if (local_size > std::numeric_limits<u32>::max()) {
        throw Shader::RuntimeError("Archive environment: invalid_local_memory");
    }
    local_memory_size = static_cast<u32>(local_size);
    if (constant_size) {
        const auto section = program.subspan(constant_offset, constant_size);
        constants.assign(section.begin(), section.end());
    }

    LowerProgram(program, HeaderSize, code_size, stage == Shader::Stage::Geometry);
}

void ArchiveEnvironment::LowerProgram(std::span<const u8> program, size_t code_begin,
                                      size_t code_end, bool geometry) {
    using namespace Shader::Ampere;
    std::vector<Instruction> native;
    ProgramCaptureState capture;
    bool complete = false;
    for (size_t offset = code_begin; offset < code_end; offset += sizeof(Instruction)) {
        if (native.size() == ProgramCaptureState::MaximumInstructions) {
            throw Shader::RuntimeError("Archive environment: program_limit");
        }
        Instruction instruction{};
        for (size_t i = 0; i < instruction.words.size(); ++i) {
            instruction.words[i] = Word(program, offset + i * sizeof(u32));
        }
        if (instruction.Bits(0, 12) == 0x95c) {
            throw Shader::RuntimeError("Archive environment: trap_guard");
        }
        native.push_back(instruction);
        const auto status = capture.Observe(instruction, static_cast<u32>(native.size() - 1));
        if (status == ProgramCaptureState::Result::InvalidTarget) {
            throw Shader::RuntimeError("Archive environment: invalid_target");
        }
        if (status == ProgramCaptureState::Result::Complete) {
            complete = true;
            break;
        }
    }
    if (!complete) {
        throw Shader::RuntimeError("Archive environment: incomplete_program");
    }
    auto lowered = LowerToMaxwell(native, {.geometry = geometry, .tensor = true,
                                           .native_code_address = 0xfffffff0ULL});
    if (!lowered) {
        throw Shader::RuntimeError("Archive environment: unsupported_lowering ({})", lowered.error);
    }
    const auto relocated = LowerToMaxwell(native, {.geometry = geometry, .tensor = true,
                                                   .native_code_address = 0x200000010ULL});
    if (!relocated || lowered.instructions != relocated.instructions ||
        lowered.source_instruction_offsets != relocated.source_instruction_offsets ||
        lowered.source_size != relocated.source_size ||
        lowered.required_texture_buffer != relocated.required_texture_buffer ||
        lowered.high_uniform_registers != relocated.high_uniform_registers) {
        throw Shader::RuntimeError("Archive environment: base_dependent");
    }
    auto result = std::make_shared<LoweredNativeCode>();
    result->native_program_key = NativeProgramKey(native);
    result->texture_bound = lowered.required_texture_buffer;
    result->source_size = code_begin + lowered.source_size;
    result->code.resize(sizeof(sph) / sizeof(u64));
    std::memcpy(result->code.data(), &sph, sizeof(sph));
    for (const u64 instruction : lowered.instructions) {
        if ((result->code.size() * sizeof(u64)) % 32 == 0) {
            result->code.push_back(0);
        }
        result->code.push_back(instruction);
    }
    result->code.push_back(0);
    AppendNativeAddressMap(result->code, lowered.source_instruction_offsets);
    InitializeNativeCode(std::move(result), static_cast<u32>(code_begin), geometry);
}

ArchiveEnvironment::ComputeState ArchiveEnvironment::ReadComputeState(std::span<const u8> control) {
    // Words 0x58..0x6b of the control section: block dimensions, the shared memory the program
    // declares and its per-thread local memory. The QMD the game's driver builds from them
    // carries the shared size rounded up to 256 bytes (checked against recorded launches).
    if (control.size() < 0x6c || Word(control, 0) != 0x98761234 || Word(control, 4) != 0xc9 ||
        Word(control, 8) != 20) {
        throw Shader::RuntimeError("Archive environment: unsupported_control");
    }
    ComputeState state;
    u64 invocations = 1;
    for (size_t i = 0; i < state.workgroup_size.size(); ++i) {
        state.workgroup_size[i] = Word(control, 0x58 + i * sizeof(u32));
        invocations *= state.workgroup_size[i];
    }
    if (invocations == 0 || invocations > 1024) {
        throw Shader::RuntimeError("Archive environment: invalid_workgroup");
    }
    const u32 shared = Word(control, 0x64);
    if (shared > 0x40000) {
        throw Shader::RuntimeError("Archive environment: invalid_workgroup (shared memory)");
    }
    state.shared_memory_size = (shared + 0xff) & ~0xffU;
    state.local_memory_size = Word(control, 0x68);
    return state;
}

ArchiveEnvironment::ArchiveEnvironment(ComputeProgram, std::span<const u8> program,
                                       std::span<const u8> control, u32 texture_buffer,
                                       Shader::TextureType texture_type_)
    : texture_type{texture_type_} {
    using namespace Shader::Ampere;
    tracks_texture_handles = true;
    const ComputeState state = ReadComputeState(control);
    if (control[0x56] != 5) {
        throw Shader::RuntimeError("Archive environment: stage_mismatch");
    }
    const u32 code_size = Word(control, 0x38);
    const u32 constant_size = Word(control, 0x3c);
    const u32 constant_offset = Word(control, 0x40);
    if (code_size < sizeof(Instruction) || code_size > program.size() ||
        code_size % sizeof(Instruction) != 0 || constant_size > 0x100 ||
        (constant_size && constant_offset < code_size)) {
        throw Shader::RuntimeError("Archive environment: invalid_range");
    }
    stage = Shader::Stage::Compute;
    is_proprietary_driver = true;
    texture_bound = texture_buffer;
    initial_offset = sizeof(sph);
    start_address = sizeof(sph);
    workgroup_size = state.workgroup_size;
    shared_memory_size = state.shared_memory_size;
    local_memory_size = state.local_memory_size;
    // A section past the extracted bytes stays absent: bank-1 reads fail as missing_constant.
    if (constant_size && constant_offset <= program.size() &&
        constant_size <= program.size() - constant_offset) {
        const auto section = program.subspan(constant_offset, constant_size);
        constants.assign(section.begin(), section.end());
    }
    LowerProgram(program, 0, code_size, false);
}

u32 ArchiveEnvironment::ExactWord(u32 index, u32 offset) const {
    if (index != 1 || offset > constants.size() || constants.size() - offset < sizeof(u32)) {
        throw Shader::RuntimeError("Archive environment: missing_constant (bank={}, offset={:#x})",
                                   index, offset);
    }
    return Word(constants, offset);
}

u32 ArchiveEnvironment::ReadCbufValue(u32 index, u32 offset) {
    if (index == 1 && offset <= constants.size() && constants.size() - offset >= sizeof(u32)) {
        return ExactWord(index, offset);
    }
    if (handle_depth != 0) {
        // Copied from ReplayEnvironment's guess mode: distinct words preserve handle provenance.
        const auto [it, inserted] = guessed_words.try_emplace(
            std::pair{index, offset}, static_cast<u32>(guessed_words.size() + 1));
        return it->second;
    }
    return ExactWord(index, offset);
}

void ArchiveEnvironment::NoteExactCbufWord(u32 index, u32 offset) {
    (void)ExactWord(index, offset);
}

void ArchiveEnvironment::BeginTextureHandle() {
    ++handle_depth;
}

void ArchiveEnvironment::EndTextureHandle(const Shader::ConstBufferAddr&, u32) {
    if (handle_depth != 0) {
        --handle_depth;
    }
}

Shader::TextureType ArchiveEnvironment::ReadTextureType(u32) {
    return texture_type;
}
Shader::TexturePixelFormat ArchiveEnvironment::ReadTexturePixelFormat(u32) {
    return Shader::TexturePixelFormat::A8B8G8R8_UNORM;
}
bool ArchiveEnvironment::IsTexturePixelFormatInteger(u32) {
    return false;
}
u32 ArchiveEnvironment::ReadViewportTransformState() {
    return 1;
}
std::optional<Shader::ReplaceConstant> ArchiveEnvironment::GetReplaceConstBuffer(u32, u32) {
    return std::nullopt;
}
void ArchiveEnvironment::SetStoragePointerAnswers(
    std::span<const TranslationCache::Query> answers) {
    using TranslationCache::QueryKind;
    pointer_answers.clear();
    for (const auto& query : answers) {
        if (query.kind == QueryKind::CbufStorageBufferPointer)
            pointer_answers[{query.a, query.b, 0, false}] = query.answer != 0;
        else if (query.kind == QueryKind::CbufIndirectStorageBufferPointer)
            pointer_answers[{query.a, query.b, query.c, true}] = query.answer != 0;
    }
}

bool ArchiveEnvironment::IsCbufStorageBufferPointer(u32 index, u32 offset) {
    if (const auto it = pointer_answers.find({index, offset, 0, false}); it != pointer_answers.end())
        return it->second;
    return true;
}
bool ArchiveEnvironment::IsCbufIndirectStorageBufferPointer(u32 index, u32 offset, u32 indirect) {
    if (const auto it = pointer_answers.find({index, offset, indirect, true});
        it != pointer_answers.end())
        return it->second;
    return false;
}
} // namespace VideoCommon
