// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/frontend/maxwell/structured_control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "video_core/offline_recorded_producer.h"
#include "video_core/pipeline_cache_files.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/shader_environment.h"

namespace VideoCommon::OfflineShaders {
namespace {
namespace TC = TranslationCache;
constexpr std::array<char, 8> PipelineMagic{'y', 'u', 'z', 'u', 'c', 'a', 'c', 'h'};

bool PipelineFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::array<char, 8> magic{};
    return file.read(magic.data(), magic.size()) && magic == PipelineMagic;
}

void ProduceStage(FileEnvironment& env, u64 hash, u32 slot, bool dual_vertex,
                  const Shader::Profile& profile, const Shader::HostTranslateInfo& host,
                  const TC::Digest& config, TC::Store& store, const TC::RecipeCatalog* catalog,
                  u32 shared_memory_limit, RecordedOutcome& result) {
    const bool compute = env.ShaderStage() == Shader::Stage::Compute;
    const auto key = TC::MakeStageKey(env, hash, slot, config);
    // Never allow a cache hit to bypass current-translator validation of a source record.
    const auto translate = [&](TC::RecordingEnvironment& recording,
                               Shader::ObjectPool<Shader::Maxwell::Flow::Block>& flow,
                               Shader::ObjectPool<Shader::IR::Inst>& inst,
                               Shader::ObjectPool<Shader::IR::Block>& blocks) {
        const u32 start = env.StartAddress() + (compute ? 0 : sizeof(Shader::ProgramHeader));
        Shader::Maxwell::Flow::CFG cfg(recording, flow, start, !compute && slot == 0);
        return Shader::Maxwell::TranslateProgram(inst, blocks, recording, cfg, host);
    };
    TC::RecordingEnvironment recording(env, true);
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow;
    Shader::ObjectPool<Shader::IR::Inst> inst;
    Shader::ObjectPool<Shader::IR::Block> blocks;
    auto program = translate(recording, flow, inst, blocks);
    ++result.stages;
    ++result.stages_by_type[static_cast<size_t>(program.stage)];
    // Dual vertex entries must describe the merged IR, and layer emulation needs a generated
    // geometry stage. Validate these records but leave emission to the runtime.
    if (dual_vertex || program.info.requires_layer_emulation) return;
    if (!recording.CanCacheTranslation()) throw std::runtime_error("uncacheable_recorded_stage");
    const auto queries = recording.TakeQueries();
    // Insert uses the complete transcript identity; replaying an older, shorter transcript is
    // not sufficient when the current translator has added another question.
    const auto inserted = store.Insert(key, queries, TC::ProgramMeta::From(program), true);
    if (!inserted) throw std::runtime_error("cache_disabled");
    const auto entry = inserted;

    std::vector<TC::EmissionRecipe> recipes;
    if (compute) {
        recipes.emplace_back();
    } else if (catalog) {
        const auto source_key = TC::MakeStageKey(env, hash, slot, catalog->source_config);
        recipes = catalog->Select(source_key, program.stage, program.info,
                                  TC::DefaultRecipeCount, TC::RecipeBaseline::Always);
    } else {
        recipes.push_back(TC::DefaultEmissionRecipe(program.stage));
    }
    for (auto recipe : recipes) {
        recipe.runtime = TC::CanonicalRuntimeInfo(recipe.runtime, program.info, program.stage);
        const auto variant = TC::MakeVariantKey(entry->id,
            compute ? TC::Digest{} : TC::DigestRuntimeInfo(recipe.runtime), recipe.bindings);
        const auto state = store.ValidateVariantForResume(variant);
        if (state == TC::Store::ResumeVariant::Ready ||
            state == TC::Store::ResumeVariant::Validated) {
            ++result.resumed;
            continue;
        }
        // Emission mutates the IR, so each recipe gets a fresh translation.
        TC::RecordingEnvironment fresh_recording(env, true);
        Shader::ObjectPool<Shader::Maxwell::Flow::Block> fresh_flow;
        Shader::ObjectPool<Shader::IR::Inst> fresh_inst;
        Shader::ObjectPool<Shader::IR::Block> fresh_blocks;
        auto fresh = translate(fresh_recording, fresh_flow, fresh_inst, fresh_blocks);
        if (compute && shared_memory_limit && fresh.shared_memory_size > shared_memory_limit)
            fresh.shared_memory_size = shared_memory_limit;
        if (!compute) Shader::Maxwell::ConvertLegacyToGeneric(fresh, recipe.runtime);
        auto binding = recipe.bindings;
        auto code = compute ? Shader::Backend::SPIRV::EmitSPIRV(profile, fresh)
                            : Shader::Backend::SPIRV::EmitSPIRV(profile, recipe.runtime, fresh, binding);
        if (code.size() < 5 || code.front() != 0x07230203)
            throw std::runtime_error("recorded_emission_rejected");
        if (!store.InsertVariant(*entry, variant, {binding, fresh.info, code, true},
                                 compute ? nullptr : &recipe))
            throw std::runtime_error("cache_disabled");
        ++result.modules;
        result.module_bytes += code.size() * sizeof(u32);
    }
}
} // namespace

RecordedOutcome ProduceRecorded(const std::filesystem::path& source,
    const Shader::Profile& profile, const Shader::HostTranslateInfo& host,
    const TranslationCache::Digest& config, TranslationCache::Store& store,
    const TranslationCache::RecipeCatalog* recipes, u32 shared_memory_limit) {
    RecordedOutcome result;
    const auto entry_count = [&] {
        u64 total{};
        for (const auto& [modules, entries] : store.VariantMultiplicity()) total += entries;
        return total;
    };
    const auto initial_entries = entry_count();
    std::vector<std::filesystem::path> paths;
    if (std::filesystem::is_directory(source)) {
        for (const auto& item : std::filesystem::recursive_directory_iterator(source))
            if (item.is_regular_file() && PipelineFile(item.path())) paths.push_back(item.path());
        std::ranges::sort(paths);
    } else {
        paths.push_back(source);
    }
    for (const bool replay_only : {false, true}) {
        for (const auto& path : paths) {
            u64 record{};
            try {
                std::ifstream file(path, std::ios::binary | std::ios::ate);
                file.exceptions(std::ios::failbit | std::ios::badbit);
                const auto end = file.tellg();
                file.seekg(0);
                std::array<char, 8> magic{};
                u32 version{};
                file.read(magic.data(), magic.size());
                file.read(reinterpret_cast<char*>(&version), sizeof(version));
                if (magic != PipelineMagic || version != VulkanPipelineCacheVersion)
                    throw std::runtime_error("invalid_pipeline_cache_header");
                if (!replay_only) ++result.files;
                while (file.tellg() != end) {
                    if (StopSignal()) {
                        result.entries = entry_count() - initial_entries;
                        return result;
                    }
                    ++record;
                    u32 count{};
                    file.read(reinterpret_cast<char*>(&count), sizeof(count));
                    if (count == 0 || count > 6) throw std::runtime_error("invalid_environment_count");
                    std::vector<FileEnvironment> envs(count);
                    for (auto& env : envs) env.Deserialize(file);
                    std::array<u64, 6> hashes{};
                    u64 pipeline{};
                    const bool compute = envs.front().ShaderStage() == Shader::Stage::Compute;
                    if (compute) {
                        Vulkan::ComputePipelineCacheKey key{};
                        file.read(reinterpret_cast<char*>(&key), sizeof(key));
                        hashes[5] = key.unique_hash;
                        pipeline = key.Hash();
                    } else {
                        Vulkan::GraphicsPipelineCacheKey key{};
                        file.read(reinterpret_cast<char*>(&key), sizeof(key));
                        hashes = key.unique_hashes;
                        pipeline = key.Hash();
                    }
                    if (std::ranges::count_if(hashes, [](u64 hash) { return hash != 0; }) != count)
                        throw std::runtime_error("pipeline_environment_count_mismatch");
                    if (!replay_only) ++result.records;
                    bool failed{};
                    size_t env_index{};
                    for (u32 slot = 0; slot < hashes.size(); ++slot) {
                        if (!hashes[slot]) continue;
                        auto& env = envs[env_index++];
                        try {
                            if (static_cast<size_t>(env.ShaderStage()) >= result.stages_by_type.size())
                                throw std::runtime_error("invalid_shader_stage");
                            if (replay_only) {
                                const auto key = TC::MakeStageKey(env, hashes[slot], slot, config);
                                const auto counts = store.ReplayAllEntries(key, env);
                                result.replay.matched += counts.matched;
                                result.replay.mismatched += counts.mismatched;
                                result.replay.unavailable += counts.unavailable;
                                if (!counts.matched)
                                    ++result.uncovered_by_type[static_cast<size_t>(env.ShaderStage())];
                                continue;
                            }
                            ProduceStage(env, hashes[slot], slot, !compute && hashes[0] != 0 && slot < 2,
                                         profile, host, config, store, recipes, shared_memory_limit, result);
                        } catch (const std::exception& error) {
                            failed = true;
                            if (!replay_only) result.failures.push_back({path, record, pipeline, slot, hashes[slot], error.what()});
                        }
                    }
                    if (!replay_only) result.failed_records += failed;
                }
            } catch (const std::exception& error) {
                if (!replay_only) result.failures.push_back({path, record, 0, 0, 0, error.what()});
            }
        }
    }
    result.entries = entry_count() - initial_entries;
    return result;
}
} // namespace VideoCommon::OfflineShaders
