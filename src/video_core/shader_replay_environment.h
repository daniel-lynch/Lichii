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
    /// With `guess`, a query the answers do not cover is answered with the common case
    /// instead of throwing (ahead-of-time translation; the draw's replay validates the guess):
    /// a distinct made-up word inside a texture handle, a 2D float texture, a direct storage
    /// pointer, no indirect pointer table, the viewport transform on and no HLE replacement.
    /// A bank-1 word is answered from `constants` (the program's constant section); another
    /// constant buffer word consumed by value cannot be guessed and still throws.
    explicit ReplayEnvironment(const GenericEnvironment& live,
                               std::span<const TranslationCache::Query> queries,
                               bool guess = false, std::span<const u8> constants = {});
    ~ReplayEnvironment() override;

    u32 ReadCbufValue(u32 cbuf_index, u32 cbuf_offset) override;
    Shader::TextureType ReadTextureType(u32 handle) override;
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32 handle) override;
    bool IsTexturePixelFormatInteger(u32 handle) override;
    u32 ReadViewportTransformState() override;
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32 bank, u32 offset) override;
    bool IsCbufStorageBufferPointer(u32 index, u32 offset) override;
    bool IsCbufIndirectStorageBufferPointer(u32 index, u32 offset, u32 indirect) override;

    void BeginTextureHandle() override {
        ++handle_depth;
    }
    void EndTextureHandle(const Shader::ConstBufferAddr&, u32) override {
        if (handle_depth != 0) {
            --handle_depth;
        }
    }

    /// Approximate heap bytes this copy owns (code, lowered program, answers, constants).
    [[nodiscard]] size_t OwnedBytes() const noexcept {
        size_t bytes = sizeof(*this) + code.capacity() * sizeof(u64) + constants.capacity() +
                       answers.size() * 64;
        if (lowered_native) {
            bytes += lowered_native->code.capacity() * sizeof(u64) +
                     lowered_native->trap_prefix.capacity() * sizeof(Shader::Ampere::Instruction);
        }
        return bytes;
    }

    /// The kind of the first query the recorded answers did not cover.
    [[nodiscard]] std::optional<TranslationCache::QueryKind> FirstMissing() const noexcept {
        return first_missing;
    }

private:
    u32 Answer(TranslationCache::QueryKind kind, u32 a, u32 b, u32 c = 0) const;

    std::map<std::tuple<u8, u32, u32, u32>, u32> answers;
    mutable std::optional<TranslationCache::QueryKind> first_missing;
    bool guess{};
    u32 handle_depth{};
    std::map<std::pair<u32, u32>, u32> guessed_words;
    /// Guess mode: the bank-1 constant section, answered exactly.
    std::vector<u8> constants;
};

} // namespace VideoCommon
