// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <map>
#include "video_core/shader_environment.h"

namespace VideoCommon {

// Owned inputs: no guest memory, GPU state or file handles survive construction.
class ArchiveEnvironment final : public GenericEnvironment {
public:
    // Program includes the 128-byte native graphics header; control is its archive control
    // section. Initially only vertex, fragment and geometry have supported scalar recipes.
    ArchiveEnvironment(std::span<const u8> program, std::span<const u8> control,
                       Shader::Stage shader_stage, u32 texture_buffer = 0);

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

private:
    u32 ExactWord(u32 index, u32 offset) const;
    std::vector<u8> constants;
    std::map<std::pair<u32, u32>, u32> guessed_words;
    u32 handle_depth{};
};

} // namespace VideoCommon
