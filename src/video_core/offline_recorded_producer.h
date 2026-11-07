// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "video_core/offline_shader_producer.h"

namespace VideoCommon::OfflineShaders {
struct RecordedFailure {
    std::filesystem::path source;
    u64 record{};
    u64 pipeline{};
    u32 slot{};
    u64 shader{};
    std::string error;
};
struct RecordedOutcome {
    u64 files{}, records{}, failed_records{}, stages{}, entries{}, modules{}, module_bytes{}, resumed{};
    TranslationCache::Store::ReplayCounts replay{};
    std::array<u64, static_cast<size_t>(Shader::Stage::Mesh) + 1> stages_by_type{}, uncovered_by_type{};
    std::vector<RecordedFailure> failures;
};

/// Read-only pipeline-cache validation: every environment is translated again, even on resume.
/// Successful stages supply replay-guarded entries, baseline modules and imported recipes.
[[nodiscard]] RecordedOutcome ProduceRecorded(
    const std::filesystem::path& source, const Shader::Profile& profile,
    const Shader::HostTranslateInfo& host, const TranslationCache::Digest& config,
    TranslationCache::Store& store, const TranslationCache::RecipeCatalog* recipes = nullptr,
    u32 shared_memory_limit = 0);
} // namespace VideoCommon::OfflineShaders
