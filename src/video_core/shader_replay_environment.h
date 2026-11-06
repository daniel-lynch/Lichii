// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <map>
#include <span>
#include <tuple>

#include "video_core/shader_environment.h"
#include "video_core/shader_translation_cache.h"

namespace VideoCommon {

/// An owned copy of a stage's environment for translating it again on a worker: the code, header
/// and scalar inputs are copied from the live environment, and every query is answered from the
/// translation cache entry whose replay just matched the live environment. It never reads guest
/// memory or engine state; a query the entry does not answer throws (the translation would differ).
class ReplayEnvironment final : public GenericEnvironment {
public:
    explicit ReplayEnvironment(const GenericEnvironment& live,
                               std::span<const TranslationCache::Query> queries);
    ~ReplayEnvironment() override;

    u32 ReadCbufValue(u32 cbuf_index, u32 cbuf_offset) override;
    Shader::TextureType ReadTextureType(u32 handle) override;
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32 handle) override;
    bool IsTexturePixelFormatInteger(u32 handle) override;
    u32 ReadViewportTransformState() override;
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32 bank, u32 offset) override;
    bool IsCbufStorageBufferPointer(u32 index, u32 offset) override;
    bool IsCbufIndirectStorageBufferPointer(u32 index, u32 offset, u32 indirect) override;

    /// The kind of the first query the recorded answers did not cover.
    [[nodiscard]] std::optional<TranslationCache::QueryKind> FirstMissing() const noexcept {
        return first_missing;
    }

private:
    u32 Answer(TranslationCache::QueryKind kind, u32 a, u32 b, u32 c = 0) const;

    std::map<std::tuple<u8, u32, u32, u32>, u32> answers;
    mutable std::optional<TranslationCache::QueryKind> first_missing;
};

} // namespace VideoCommon
