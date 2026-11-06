// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include "video_core/shader_translation_cache.h"

namespace VideoCommon::OfflineShaders {
constexpr u64 MaxProgramBytes = 16 * 1024 * 1024;
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
};
struct Input {
    u64 title_id{};
    std::vector<Program> programs;
};
struct Summary {
    u64 seen{}, translated{}, resumed{}, modules{};
    std::map<std::string, u64> skipped;
};
[[nodiscard]] Input ParseInput(std::string_view json, const std::filesystem::path& directory);
[[nodiscard]] Summary Run(const std::filesystem::path& input, u64 title_id, u32 workers,
                          const std::filesystem::path& recipe_cache,
                          const Shader::Profile& profile, const Shader::HostTranslateInfo& host,
                          const TranslationCache::Digest& config, TranslationCache::Store& store);
} // namespace VideoCommon::OfflineShaders
