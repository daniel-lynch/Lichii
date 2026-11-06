// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <bitset>
#include <filesystem>
#include <iosfwd>
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
    /// Recipes a full class turned away, tallied per class (not written to the file).
    static constexpr size_t MaxShadow = 4;
    static constexpr size_t MaxClasses = 4096;
    /// Recipes whose runtime info maps more legacy varyings are not kept (a format bound).
    static constexpr size_t MaxLegacyMappings = 64;
    /// The largest file the format allows: every class full of the largest recipes.
    [[nodiscard]] static u64 MaxFileBytes();

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
    /// its first recipe. A full class replaces its weakest recipe not seen this session (from
    /// the file), else a recipe seen only once; otherwise the newcomer is tallied aside and
    /// replaces the weakest resident once its tally exceeds that resident's count, so a new
    /// dominant recipe always gets in.
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
    /// Replaces the histogram with `size` bytes of a file read from `in` (nothing is allocated
    /// for the whole file); counts from earlier sessions weigh half. A size above MaxFileBytes,
    /// a repeated class or a repeated recipe in a class is refused. On failure the histogram is
    /// left empty and `why` says what was wrong.
    bool Load(std::istream& in, u64 size, const TranslationCache::Digest& config,
              std::string& why);
    bool Load(std::span<const u8> data, const TranslationCache::Digest& config, std::string& why);

private:
    struct Class {
        Shader::Stage stage{};
        bool by_loads{};
        std::bitset<512> loads{};
        std::vector<Recipe> recipes;
        std::vector<Recipe> shadow; ///< Turned away while full, with their tallies
    };
    static std::vector<Recipe>::iterator Victim(std::vector<Recipe>& list);
    std::unordered_map<u64, Class> classes;
    u64 revision{};
};

/// Replaces `path` with `data` atomically: the bytes go to a temporary file next to it, created
/// exclusively under a name unique to this writer, which is flushed, synced, closed and renamed
/// over `path`. Two writers each publish an intact file (the last rename wins). On failure the
/// temporary file is removed, `path` is untouched and `why` says what failed.
bool WriteFileAtomically(const std::filesystem::path& path, std::span<const u8> data,
                         std::string& why);

} // namespace VideoCommon
