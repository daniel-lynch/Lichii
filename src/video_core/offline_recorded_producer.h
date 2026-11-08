// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "video_core/offline_shader_producer.h"

namespace Vulkan {
struct GraphicsPipelineCacheKey;
/// Shared by the runtime graphics builder and offline recorded-key production.
[[nodiscard]] Shader::RuntimeInfo MakeGraphicsRuntimeInfo(
    std::span<const Shader::IR::Program> programs, const GraphicsPipelineCacheKey& key,
    const Shader::IR::Program& program, const Shader::IR::Program* previous_program,
    bool supports_transform_feedback, bool molten_vk);
}

namespace VideoCommon {
class FileEnvironment;
}

namespace VideoCommon::OfflineShaders {
struct RecordedStageLookup {
    u32 slot{};
    u64 shader{};
    Shader::Stage stage{};
    TranslationCache::Digest stage_key{}, entry{}, variant{};
    bool hit{};
    std::string reason;
};
struct RecordedKeyLookup {
    std::filesystem::path source;
    u64 record{}, pipeline{};
    std::vector<RecordedStageLookup> stages;
};
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
    bool exact_validation{}, validation_reloaded{};
    std::vector<RecordedKeyLookup> lookups;
};

/// Read-only pipeline-cache validation: every environment is translated again, even on resume.
/// Successful stages supply replay-guarded entries, baseline modules and imported recipes.
[[nodiscard]] RecordedOutcome ProduceRecorded(
    const std::filesystem::path& source, const Shader::Profile& profile,
    const Shader::HostTranslateInfo& host, const TranslationCache::Digest& config,
    TranslationCache::Store& store, const TranslationCache::RecipeCatalog* recipes = nullptr,
    u32 shared_memory_limit = 0, RecordedOptions options = {});

/// Replay the runtime's entry and variant selection without adding modules to the store.
[[nodiscard]] RecordedKeyLookup ValidateRecordedGraphics(
    const Vulkan::GraphicsPipelineCacheKey& key, std::span<FileEnvironment> envs,
    const Shader::Profile& profile, const Shader::HostTranslateInfo& host,
    const TranslationCache::Digest& config, TranslationCache::Store& store,
    RecordedOptions options = {});
[[nodiscard]] std::string RecordedReport(const RecordedOutcome& result);
} // namespace VideoCommon::OfflineShaders
