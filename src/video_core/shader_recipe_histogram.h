// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <bitset>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/runtime_info.h"
#include "shader_recompiler/stage.h"
#include "video_core/shader_translation_cache.h"

namespace VideoCommon {

/// Shader AOT seeded variants: the emission recipes (runtime info and bindings in) seen at run
/// time, by class (stage and loaded varyings) and by stage. A seed gets the module of its class's
/// most common recipe, else its stage's. Not thread safe: the pipeline cache locks it.
///
/// The histogram is also kept on disk next to the translation file, so a later session guesses
/// from the start (LXRECIP1: magic, format version, translation configuration digest, then per
/// class its identity and recipes).
class RecipeHistogram {
public:
    static constexpr size_t MaxPerClass = 8;
    static constexpr size_t MaxClasses = 4096;

    struct Recipe {
        Shader::RuntimeInfo runtime_info;
        Shader::Backend::Bindings binding_in{};
        TranslationCache::Digest digest{};
        u64 count{};
        bool seen{}; ///< Recorded this session (false: only from the file)
    };

    /// The class key of a stage's programs that load `loads`, or of the whole stage (null).
    [[nodiscard]] static u64 ClassKey(Shader::Stage stage, const std::bitset<512>* loads);

    /// A run-time stage was emitted with this recipe. Returns whether its class (by loads) got
    /// its first recipe.
    bool Record(Shader::Stage stage, const std::bitset<512>& loads,
                const Shader::RuntimeInfo& runtime_info,
                const Shader::Backend::Bindings& binding_in);

    /// The most common recipe of the class, else of the stage; `class_match` says which.
    [[nodiscard]] std::optional<Recipe> Best(Shader::Stage stage, const std::bitset<512>& loads,
                                             bool& class_match) const;

    /// Whether the class has a recipe.
    [[nodiscard]] bool HasRecipes(u64 class_key) const;

    void Clear();
    [[nodiscard]] size_t Classes() const noexcept {
        return classes.size();
    }
    [[nodiscard]] size_t Recipes() const noexcept;
    /// Bumped by every change (recording or loading).
    [[nodiscard]] u64 Revision() const noexcept {
        return revision;
    }

    /// The file contents for this translation configuration.
    [[nodiscard]] std::vector<u8> Serialize(const TranslationCache::Digest& config) const;
    /// Replaces the histogram with a file's contents; counts from earlier sessions weigh half.
    /// On failure the histogram is left empty and `why` says what was wrong.
    bool Load(std::span<const u8> data, const TranslationCache::Digest& config, std::string& why);

private:
    struct Class {
        Shader::Stage stage{};
        bool by_loads{};
        std::bitset<512> loads{};
        std::vector<Recipe> recipes;
    };
    std::unordered_map<u64, Class> classes;
    u64 revision{};
};

} // namespace VideoCommon
