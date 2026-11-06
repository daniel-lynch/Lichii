// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "shader_recompiler/exception.h"
#include "video_core/shader_replay_environment.h"

namespace VideoCommon {

using TranslationCache::QueryKind;

ReplayEnvironment::ReplayEnvironment(const GenericEnvironment& live,
                                     std::span<const TranslationCache::Query> queries)
    : GenericEnvironment(live) {
    gpu_memory = nullptr;
    for (const auto& query : queries) {
        answers.emplace(std::tuple{static_cast<u8>(query.kind), query.a, query.b, query.c},
                        query.answer);
    }
}

ReplayEnvironment::~ReplayEnvironment() = default;

u32 ReplayEnvironment::Answer(QueryKind kind, u32 a, u32 b, u32 c) const {
    const auto it = answers.find(std::tuple{static_cast<u8>(kind), a, b, c});
    if (it == answers.end()) {
        if (!first_missing) {
            first_missing = kind;
        }
        throw Shader::LogicError("Replay environment: query {} ({:#x}, {:#x}, {:#x}) not recorded",
                                 static_cast<u32>(kind), a, b, c);
    }
    return it->second;
}

u32 ReplayEnvironment::ReadCbufValue(u32 cbuf_index, u32 cbuf_offset) {
    return Answer(QueryKind::CbufValue, cbuf_index, cbuf_offset);
}

Shader::TextureType ReplayEnvironment::ReadTextureType(u32 handle) {
    return static_cast<Shader::TextureType>(Answer(QueryKind::TextureType, handle, 0));
}

Shader::TexturePixelFormat ReplayEnvironment::ReadTexturePixelFormat(u32 handle) {
    return static_cast<Shader::TexturePixelFormat>(
        Answer(QueryKind::TexturePixelFormat, handle, 0));
}

bool ReplayEnvironment::IsTexturePixelFormatInteger(u32 handle) {
    return Answer(QueryKind::TexturePixelFormatInteger, handle, 0) != 0;
}

u32 ReplayEnvironment::ReadViewportTransformState() {
    return Answer(QueryKind::ViewportTransformState, 0, 0);
}

std::optional<Shader::ReplaceConstant> ReplayEnvironment::GetReplaceConstBuffer(u32 bank,
                                                                                u32 offset) {
    const u32 value = Answer(QueryKind::ReplaceConstBuffer, bank, offset);
    if (value == ~0U) {
        return std::nullopt;
    }
    return static_cast<Shader::ReplaceConstant>(value);
}

bool ReplayEnvironment::IsCbufStorageBufferPointer(u32 index, u32 offset) {
    return Answer(QueryKind::CbufStorageBufferPointer, index, offset) != 0;
}

bool ReplayEnvironment::IsCbufIndirectStorageBufferPointer(u32 index, u32 offset, u32 indirect) {
    return Answer(QueryKind::CbufIndirectStorageBufferPointer, index, offset, indirect) != 0;
}

} // namespace VideoCommon
