// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
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

RecordedKeyLookup ProcessGraphics(const Vulkan::GraphicsPipelineCacheKey& key,
    std::span<FileEnvironment> envs, const Shader::Profile& profile,
    const Shader::HostTranslateInfo& host, const TC::Digest& config, TC::Store& store,
    RecordedOptions options, RecordedOutcome* production) {
    RecordedKeyLookup lookup{.pipeline = key.Hash()};
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow;
    Shader::ObjectPool<Shader::IR::Inst> inst;
    Shader::ObjectPool<Shader::IR::Block> blocks;
    std::array<Shader::IR::Program, 6> programs, selected_programs;
    std::array<std::shared_ptr<const TC::Entry>, 6> entries;
    size_t env_index{};
    bool layer_emulation{};
    for (u32 slot = 0; slot < key.unique_hashes.size(); ++slot) {
        if (!key.unique_hashes[slot]) continue;
        if (env_index >= envs.size()) throw std::runtime_error("pipeline_environment_count_mismatch");
        auto& env = envs[env_index++];
        auto& status = lookup.stages.emplace_back();
        status.slot = slot;
        status.shader = key.unique_hashes[slot];
        status.stage = env.ShaderStage();
        try {
            const auto stage_key = TC::MakeStageKey(env, status.shader, slot, config);
            status.stage_key = stage_key;
            TC::RecordingEnvironment recording(env, true);
            Shader::Maxwell::Flow::CFG cfg(recording, flow,
                static_cast<u32>(env.StartAddress() + sizeof(Shader::ProgramHeader)), slot == 0);
            auto& program = programs[slot];
            program = Shader::Maxwell::TranslateProgram(inst, blocks, recording, cfg, host);
            layer_emulation |= program.info.requires_layer_emulation;
            if (production) {
                ++production->stages;
                ++production->stages_by_type.at(static_cast<size_t>(program.stage));
                if (!recording.CanCacheTranslation())
                    throw std::runtime_error("uncacheable_recorded_stage");
                // Insert the complete current transcript even when another answer set exists.
                if (!key.unique_hashes[0] && !program.info.requires_layer_emulation &&
                    !store.Insert(stage_key, recording.TakeQueries(), TC::ProgramMeta::From(program), true))
                    throw std::runtime_error("cache_disabled");
            }
            entries[slot] = store.FindEntry(stage_key, env);
            if (!entries[slot]) status.reason = "entry_miss";
            else status.entry = entries[slot]->id;
            selected_programs[slot] = entries[slot] && entries[slot]->CanReuseMetadata()
                ? entries[slot]->meta.ToProgram() : TC::ProgramMeta::From(program).ToProgram();
        } catch (const std::exception& error) {
            status.reason = error.what();
        }
    }
    if (env_index != envs.size()) throw std::runtime_error("pipeline_environment_count_mismatch");
    // Runtime deliberately disables translation-cache lookup for these pipelines.
    if (key.unique_hashes[0] || layer_emulation) {
        for (auto& status : lookup.stages)
            status.reason = key.unique_hashes[0] ? "runtime_cache_disabled_vertex_a"
                                                : "runtime_cache_disabled_layer_emulation";
        return lookup;
    }
    Shader::Backend::Bindings binding{};
    const Shader::IR::Program* previous{};
    bool predecessor_failed{};
    for (auto& status : lookup.stages) {
        auto& program = programs[status.slot];
        auto entry = entries[status.slot];
        if (predecessor_failed) {
            status.reason = "predecessor_unavailable";
            continue;
        }
        if (!status.reason.empty() && status.reason != "entry_miss") {
            predecessor_failed = true;
            continue;
        }
        try {
            // Use the metadata the runtime would select, including task/mesh primitive lines.
            // A fresh IR is retained for emission on a miss.
            auto& selected = selected_programs[status.slot];
            const auto runtime = TC::CanonicalRuntimeInfo(Vulkan::MakeGraphicsRuntimeInfo(
                selected_programs, key, selected, previous, options.supports_transform_feedback,
                options.molten_vk), selected.info, selected.stage);
            const TC::EmissionRecipe recipe{runtime, binding};
            if (entry) status.variant = TC::MakeVariantKey(entry->id, TC::DigestRuntimeInfo(runtime), binding);
            // Decode disk modules too: a directory/index entry alone is not a hit.
            if (production && entry) (void)store.ValidateVariantForResume(status.variant);
            auto variant = entry ? store.FindVariant(status.variant) : std::nullopt;
            if (variant && !selected.info.loads.Legacy() && !selected.info.stores.Legacy() &&
                !TC::InfoEqual(variant->info, selected.info)) {
                variant.reset();
                status.reason = "variant_metadata_mismatch";
            }
            if (variant) {
                selected.info = variant->info;
                binding = variant->binding_out;
                status.hit = true;
                status.reason = "hit";
                if (production) ++production->resumed;
            } else {
                if (status.reason.empty()) status.reason = "variant_miss";
                if (entry && ((entry->offline && !store.ValidateOfflineEntry(*entry, program)) ||
                    !TC::InfoEqual(program.info, selected.info) ||
                    !std::ranges::equal(program.mesh.vertex_lines, selected.mesh.vertex_lines) ||
                    !std::ranges::equal(program.mesh.primitive_lines, selected.mesh.primitive_lines)))
                    throw std::runtime_error("entry_metadata_mismatch");
                Shader::Maxwell::ConvertLegacyToGeneric(program, runtime);
                auto code = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime, program, binding);
                if (code.size() < 5 || code.front() != 0x07230203)
                    throw std::runtime_error("recorded_emission_rejected");
                if (production && entry && status.reason != "variant_metadata_mismatch") {
                    if (!store.InsertVariant(*entry, status.variant,
                            {binding, program.info, code, true}, &recipe))
                        throw std::runtime_error("cache_disabled");
                    ++production->modules;
                    production->module_bytes += code.size() * sizeof(u32);
                    status.reason = "emitted";
                }
            }
            if (!variant) selected = TC::ProgramMeta::From(program).ToProgram();
            previous = &selected;
        } catch (const std::exception& error) {
            status.reason = error.what();
            predecessor_failed = true;
        }
    }
    return lookup;
}
} // namespace

RecordedOutcome ProduceRecorded(const std::filesystem::path& source,
    const Shader::Profile& profile, const Shader::HostTranslateInfo& host,
    const TranslationCache::Digest& config, TranslationCache::Store& store,
    const TranslationCache::RecipeCatalog* recipes, u32 shared_memory_limit, RecordedOptions options) {
    RecordedOutcome result;
    result.exact_validation = options.exact_pipeline_recipes;
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
        if (replay_only && options.exact_pipeline_recipes && store.Persistent()) {
            result.validation_reloaded = store.ReloadForOfflineValidation();
            if (!result.validation_reloaded)
                throw std::runtime_error("Cannot reload offline product for exact validation");
        }
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
                        result.entries = entry_count() > initial_entries ? entry_count() - initial_entries : 0;
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
                    Vulkan::GraphicsPipelineCacheKey graphics_key{};
                    const bool compute = envs.front().ShaderStage() == Shader::Stage::Compute;
                    if (compute) {
                        Vulkan::ComputePipelineCacheKey key{};
                        file.read(reinterpret_cast<char*>(&key), sizeof(key));
                        hashes[5] = key.unique_hash;
                        pipeline = key.Hash();
                    } else {
                        auto& key = graphics_key;
                        file.read(reinterpret_cast<char*>(&key), sizeof(key));
                        hashes = key.unique_hashes;
                        pipeline = key.Hash();
                    }
                    if (std::ranges::count_if(hashes, [](u64 hash) { return hash != 0; }) != count)
                        throw std::runtime_error("pipeline_environment_count_mismatch");
                    if (!replay_only) ++result.records;
                    if (!compute && options.exact_pipeline_recipes) {
                        auto lookup = ProcessGraphics(graphics_key, envs, profile, host, config,
                                                      store, options, replay_only ? nullptr : &result);
                        lookup.source = path;
                        lookup.record = record;
                        if (replay_only) {
                            result.lookups.push_back(std::move(lookup));
                        } else {
                            bool failed{};
                            for (const auto& stage : lookup.stages) {
                                if (stage.reason == "hit" || stage.reason == "emitted") continue;
                                failed = true;
                                result.failures.push_back({path, record, pipeline, stage.slot,
                                                          stage.shader, stage.reason});
                            }
                            result.failed_records += failed;
                        }
                        if (!replay_only) continue;
                    }
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
    result.entries = entry_count() > initial_entries ? entry_count() - initial_entries : 0;
    return result;
}
RecordedKeyLookup ValidateRecordedGraphics(const Vulkan::GraphicsPipelineCacheKey& key,
    std::span<FileEnvironment> envs, const Shader::Profile& profile,
    const Shader::HostTranslateInfo& host, const TranslationCache::Digest& config,
    TranslationCache::Store& store, RecordedOptions options) {
    return ProcessGraphics(key, envs, profile, host, config, store, options, nullptr);
}

std::string RecordedReport(const RecordedOutcome& result) {
    using Json = nlohmann::json;
    Json failures = Json::array();
    for (const auto& failure : result.failures)
        failures.push_back({{"source", failure.source.string()}, {"record", failure.record},
            {"pipeline", fmt::format("{:016x}", failure.pipeline)}, {"slot", failure.slot},
            {"shader", fmt::format("{:016x}", failure.shader)}, {"error", failure.error}});
    Json keys = Json::array();
    u64 key_hits{}, key_misses{}, stage_hits{}, stage_misses{};
    std::array<u64, static_cast<size_t>(Shader::Stage::Mesh) + 1> hits_by_type{}, misses_by_type{};
    for (const auto& lookup : result.lookups) {
        Json stages = Json::array();
        bool hit = !lookup.stages.empty();
        for (const auto& stage : lookup.stages) {
            hit &= stage.hit;
            ++(stage.hit ? stage_hits : stage_misses);
            const auto type = static_cast<size_t>(stage.stage);
            if (type < hits_by_type.size()) ++(stage.hit ? hits_by_type[type] : misses_by_type[type]);
            stages.push_back({{"slot", stage.slot}, {"shader", fmt::format("{:016x}", stage.shader)},
                {"stage", type}, {"hit", stage.hit}, {"reason", stage.reason},
                {"stage_key", fmt::format("{:016x}{:016x}", stage.stage_key.hi, stage.stage_key.lo)},
                {"entry", fmt::format("{:016x}{:016x}", stage.entry.hi, stage.entry.lo)},
                {"variant", fmt::format("{:016x}{:016x}", stage.variant.hi, stage.variant.lo)}});
        }
        ++(hit ? key_hits : key_misses);
        keys.push_back({{"source", lookup.source.string()}, {"record", lookup.record},
            {"pipeline", fmt::format("{:016x}", lookup.pipeline)}, {"hit", hit}, {"stages", stages}});
    }
    return Json{{"files", result.files}, {"records", result.records},
        {"failed_records", result.failed_records}, {"stages", result.stages},
        {"entries", result.entries}, {"modules", result.modules}, {"module_bytes", result.module_bytes},
        {"stages_by_type", result.stages_by_type}, {"uncovered_by_type", result.uncovered_by_type},
        {"resumed", result.resumed}, {"replay_matched", result.replay.matched},
        {"replay_mismatched", result.replay.mismatched}, {"replay_unavailable", result.replay.unavailable},
        {"failures", failures},
        {"exact_variants", {{"enabled", result.exact_validation},
            {"reloaded_product", result.validation_reloaded}, {"key_hits", key_hits},
            {"key_misses", key_misses}, {"stage_hits", stage_hits}, {"stage_misses", stage_misses},
            {"hits_by_type", hits_by_type}, {"misses_by_type", misses_by_type}, {"keys", keys}}}}.dump();
}
} // namespace VideoCommon::OfflineShaders
