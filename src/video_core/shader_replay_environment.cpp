// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>

#include "shader_recompiler/exception.h"
#include "video_core/shader_replay_environment.h"

namespace VideoCommon {

using TranslationCache::QueryKind;

ReplayEnvironment::ReplayEnvironment(const GenericEnvironment& live,
                                     std::span<const TranslationCache::Query> queries,
                                     bool guess_, std::span<const u8> constants_)
    : GenericEnvironment(live), guess{guess_}, constants(constants_.begin(), constants_.end()) {
    gpu_memory = nullptr;
    // Guessing needs to know which words build texture handles.
    tracks_texture_handles = guess;
    for (const auto& query : queries) {
        answers.emplace(std::tuple{static_cast<u8>(query.kind), query.a, query.b, query.c},
                        query.answer);
    }
}

ReplayEnvironment::~ReplayEnvironment() = default;

u32 ReplayEnvironment::Answer(QueryKind kind, u32 a, u32 b, u32 c) const {
    const auto it = answers.find(std::tuple{static_cast<u8>(kind), a, b, c});
    if (it == answers.end() && guess) {
        switch (kind) {
        case QueryKind::TextureType:
            return static_cast<u32>(Shader::TextureType::Color2D);
        case QueryKind::TexturePixelFormat:
            return static_cast<u32>(Shader::TexturePixelFormat::A8B8G8R8_UNORM);
        case QueryKind::TexturePixelFormatInteger:
        case QueryKind::CbufIndirectStorageBufferPointer:
            return 0;
        case QueryKind::CbufStorageBufferPointer:
        case QueryKind::ViewportTransformState:
            return 1;
        case QueryKind::ReplaceConstBuffer:
            return ~0U;
        default:
            break;
        }
    }
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
    if (guess && cbuf_index == 1 && size_t{cbuf_offset} + sizeof(u32) <= constants.size()) {
        u32 word;
        std::memcpy(&word, constants.data() + cbuf_offset, sizeof(word));
        return word;
    }
    if (guess && handle_depth != 0 &&
        !answers.contains(std::tuple{static_cast<u8>(QueryKind::CbufValue), cbuf_index,
                                     cbuf_offset, 0U})) {
        // Distinct per word, so distinct handles stay distinct for the class-level replay.
        const auto [it, inserted] = guessed_words.try_emplace(
            std::pair{cbuf_index, cbuf_offset}, static_cast<u32>(guessed_words.size() + 1));
        return it->second;
    }
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
