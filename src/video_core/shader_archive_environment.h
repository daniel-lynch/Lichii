// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <map>
#include <tuple>
#include "video_core/shader_environment.h"
#include "video_core/shader_translation_cache.h"

namespace VideoCommon {

// Owned inputs: no guest memory, GPU state or file handles survive construction.
class ArchiveEnvironment final : public GenericEnvironment {
public:
    // Program includes the 128-byte native graphics header; control is its archive control
    // section. Initially only vertex, fragment and geometry have supported scalar recipes.
    ArchiveEnvironment(std::span<const u8> program, std::span<const u8> control,
                       Shader::Stage shader_stage, u32 texture_buffer = 0);

    /// The dispatch state a native compute program's control section records: what the runtime
    /// reads from the launch descriptor (QMD) when it builds the program's environment.
    struct ComputeState {
        std::array<u32, 3> workgroup_size{};
        u32 shared_memory_size{}; ///< Aligned up to the QMD's 256-byte allocation unit.
        u32 local_memory_size{};  ///< Per thread.
    };
    /// Decodes ComputeState from a compute control section; throws Shader::RuntimeError naming
    /// unsupported_control or invalid_workgroup.
    [[nodiscard]] static ComputeState ReadComputeState(std::span<const u8> control);

    struct ComputeProgram {};
    /// Native compute: program is the bare instruction stream (compute has no 128-byte header)
    /// followed by its bank-1 constants; the environment matches ComputeEnvironment in native
    /// mode (start address and initial offset past an all-zero header, proprietary driver).
    ArchiveEnvironment(ComputeProgram, std::span<const u8> program, std::span<const u8> control,
                       u32 texture_buffer = 0);

    u32 ReadCbufValue(u32 index, u32 offset) override;
    Shader::TextureType ReadTextureType(u32) override;
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override;
    bool IsTexturePixelFormatInteger(u32) override;
    u32 ReadViewportTransformState() override;
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override;
    bool IsCbufStorageBufferPointer(u32, u32) override;
    bool IsCbufIndirectStorageBufferPointer(u32, u32, u32) override;
    void BeginTextureHandle() override;
    void EndTextureHandle(const Shader::ConstBufferAddr&, u32) override;
    void NoteExactCbufWord(u32 index, u32 offset) override;
    void Dump(u64, u64) override {}

    /// Answers recorded storage-pointer queries (CbufStorageBufferPointer and
    /// CbufIndirectStorageBufferPointer) as recorded; other slots keep the guesses (direct
    /// slots hold a mapped pointer, indirect ones do not).
    void SetStoragePointerAnswers(std::span<const TranslationCache::Query> answers);

private:
    /// Captures and lowers [code_begin, code_end) of program and adopts the lowered code.
    void LowerProgram(std::span<const u8> program, size_t code_begin, size_t code_end,
                      bool geometry);
    u32 ExactWord(u32 index, u32 offset) const;
    std::vector<u8> constants;
    std::map<std::pair<u32, u32>, u32> guessed_words;
    u32 handle_depth{};
    std::map<std::tuple<u32, u32, u32, bool>, bool> pointer_answers;
};

} // namespace VideoCommon
