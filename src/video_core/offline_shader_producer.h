// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <csignal>
#include <map>
#include <string>
#include <string_view>
#include "video_core/shader_translation_cache.h"

namespace VideoCommon::OfflineShaders {
constexpr u64 MaxProgramBytes = 16 * 1024 * 1024;
/// The index is JSON text, not an allocation: a whole title's library can exceed 16 MiB.
constexpr u64 MaxIndexBytes = 64 * 1024 * 1024;
/// Largest bank-1 constant section the archive environment can read exactly.
constexpr u32 MaxBank1Bytes = 256;
struct Program {
    std::filesystem::path container;
    std::filesystem::path control;
    u64 offset{};
    u64 size{};
    u64 control_offset{};
    u32 constants_offset{};
    u32 constants_size{};
    u64 fingerprint{};
    Shader::Stage stage{};
    u32 tier{};
    std::vector<u32> bank1_words;
    /// Nonempty when the row is valid JSON but cannot be produced (e.g. bank1_too_large): the
    /// worker counts it under this reason instead of failing the whole index.
    std::string skip_reason;
};
struct Input {
    u64 title_id{};
    std::vector<Program> programs;
};
// Instantiated after SDL initialization so its quit-event handler cannot swallow production signals.
class SignalGuard {
public:
    SignalGuard();
    ~SignalGuard();
    SignalGuard(const SignalGuard&) = delete;
    SignalGuard& operator=(const SignalGuard&) = delete;
private:
    using Handler = void (*)(int);
    Handler previous_int;
    Handler previous_term;
};
[[nodiscard]] int StopSignal();
struct SourceSummary {
    u64 modules{}, module_bytes{};
};
struct Summary {
    u64 seen{}, translated{}, resumed{}, modules{}, module_bytes{};
    u64 entries{}, product_modules{};
    u64 validated_modules{}, invalidated_modules{}, replaced_modules{};
    bool cancelled{};
    std::array<SourceSummary, 3> sources{};
    // Entire current product, including modules retained from previous runs.
    std::map<u64, u64> multiplicity;
    u64 import_entries{}, import_truncated_entries{}, import_skipped_records{},
        import_bytes{}, import_invalid_recipes{}, import_scanned_records{},
        import_eligible_records{}, import_admitted_records{}, import_framing_skipped_bytes{};
    std::map<std::string, u64> skipped;
    /// Compute rows (one module each, no recipes); also counted in the totals above.
    struct {
        u64 translated{}, resumed{}, modules{}, module_bytes{};
        u64 pointer_modules{}, recorded_sets{};
    } compute;
};
void WriteReport(const std::filesystem::path& path, const Summary& summary);
[[nodiscard]] Input ParseInput(std::string_view json, const std::filesystem::path& directory);
[[nodiscard]] Summary Run(const std::filesystem::path& input, u64 title_id, u32 workers,
                          const std::filesystem::path& recipe_cache,
                          const Shader::Profile& profile, const Shader::HostTranslateInfo& host,
                          const TranslationCache::Digest& config, TranslationCache::Store& store,
                          u32 recipe_count = TranslationCache::DefaultRecipeCount,
                          u32 recipe_limit = TranslationCache::DefaultRecipeLimit,
                          const std::filesystem::path& report = {},
                          TranslationCache::RecipeBaseline baseline =
                              TranslationCache::RecipeBaseline::Auto,
                          u32 compute_shared_memory_limit = 0);
} // namespace VideoCommon::OfflineShaders
