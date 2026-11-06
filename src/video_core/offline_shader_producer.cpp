// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <fstream>
#include <unordered_set>
#include <fmt/format.h>
#include "common/polyfill_thread.h"
#include <nlohmann/json.hpp>
#include "common/logging.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/maxwell/structured_control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "video_core/offline_shader_producer.h"
#include "video_core/shader_archive_environment.h"

namespace VideoCommon::OfflineShaders {
namespace {
static_assert(std::atomic<int>::is_always_lock_free);
std::atomic<int> stop_signal{};
void StopProduction(int signal) {
    stop_signal.store(signal, std::memory_order_relaxed);
}
using Json = nlohmann::json;
u64 Unsigned(const Json& value) {
    if (!value.is_number_unsigned()) throw std::runtime_error("Expected unsigned integer");
    return value.get<u64>();
}
u32 SmallUnsigned(const Json& value) {
    const auto number = Unsigned(value);
    if (number > UINT32_MAX) throw std::runtime_error("Integer exceeds 32 bits");
    return static_cast<u32>(number);
}
u64 Hex(const Json& value) {
    const auto text = value.get<std::string>();
    u64 result{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result, 16);
    if (text.size() != 16 || error != std::errc{} || end != text.data() + text.size())
        throw std::runtime_error("Expected 16 hexadecimal digits");
    return result;
}
Shader::Stage Stage(std::string_view name) {
    static constexpr std::array names{"vertex", "tess-control", "tess-eval", "geometry",
                                      "fragment", "compute", "vertex-a", "task", "mesh"};
    const auto it = std::ranges::find(names, name);
    if (it == names.end()) throw std::runtime_error("Unknown shader stage");
    return static_cast<Shader::Stage>(it - names.begin());
}
std::vector<u8> Read(const std::filesystem::path& path, u64 offset, u64 size) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file || file.tellg() < 0) throw std::runtime_error("input_io");
    const auto length = static_cast<u64>(file.tellg());
    if (offset > length || size > length - offset || size > MaxProgramBytes)
        throw std::runtime_error("invalid_range");
    std::vector<u8> bytes(size);
    file.seekg(static_cast<std::streamoff>(offset));
    if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)))
        throw std::runtime_error("input_io");
    return bytes;
}
u32 Word(std::span<const u8> bytes, size_t offset) {
    return u32{bytes[offset]} | (u32{bytes[offset + 1]} << 8) |
           (u32{bytes[offset + 2]} << 16) | (u32{bytes[offset + 3]} << 24);
}
std::string Reason(std::string_view message) {
    for (const auto reason : {"missing_constant", "unsupported_stage", "base_dependent",
                              "bank1_too_large",
                              "unsupported_lowering", "invalid_range", "input_io",
                              "fingerprint_mismatch", "constants_mismatch"}) {
        if (message.find(reason) != std::string_view::npos) return reason;
    }
    return "lowering_or_emission_rejected";
}
} // namespace

SignalGuard::SignalGuard() {
    stop_signal.store(0, std::memory_order_relaxed);
    previous_int = std::signal(SIGINT, StopProduction);
    previous_term = std::signal(SIGTERM, StopProduction);
}
SignalGuard::~SignalGuard() {
    std::signal(SIGINT, previous_int);
    std::signal(SIGTERM, previous_term);
}
int StopSignal() {
    return stop_signal.load(std::memory_order_relaxed);
}

bool CanShareInputLayout(const Shader::Info& target, const Shader::VaryingState& observed_inputs) {
    if (target.loads.Legacy() || observed_inputs.Legacy()) return false;
    for (size_t input = 0; input < Shader::IR::NUM_GENERICS; ++input)
        if ((target.loads.Generic(input) || target.passthrough.Generic(input)) &&
            !observed_inputs.Generic(input)) return false;
    return true;
}

std::vector<TranslationCache::SelectedRecipe> ExpandRecipes(
    std::vector<TranslationCache::SelectedRecipe> selected, Shader::Stage stage,
    const Shader::Info& info, std::span<const PartnerStage> partners,
    std::span<const TranslationCache::EmissionRecipe> layouts) {
    namespace TC = TranslationCache;
    std::unordered_set<TC::Digest, TC::DigestHash> seen;
    auto key = [&](const TC::EmissionRecipe& recipe) {
        return TC::MakeVariantKey({}, TC::DigestRuntimeInfo(
            TC::CanonicalRuntimeInfo(recipe.runtime, info, stage)), recipe.bindings);
    };
    for (const auto& selection : selected) seen.insert(key(selection.recipe));
    auto add = [&](TC::EmissionRecipe recipe, TC::RecipeSource source) {
        if (seen.insert(key(recipe)).second) selected.push_back({std::move(recipe), source});
    };
    // Keep recorded draw state (including early Z), replacing only partner/layout inputs.
    auto bases = selected;
    if (bases.empty()) bases.push_back({TC::DefaultEmissionRecipe(stage), TC::RecipeSource::Baseline});
    if (stage == Shader::Stage::Fragment) {
        for (const auto& partner : partners) {
            // One recipe per partner, using the highest-ranked recorded draw state.
            auto recipe = bases.front().recipe;
            recipe.bindings = partner.bindings;
            recipe.runtime.previous_stage_stores = partner.info.stores;
            recipe.runtime.previous_stage_legacy_stores_mapping = partner.info.legacy_stores_mapping;
            add(std::move(recipe), TC::RecipeSource::PartnerStage);
        }
    } else if (stage == Shader::Stage::VertexB) {
        for (const auto& base : bases) {
            for (const auto& layout : layouts) {
                auto recipe = base.recipe;
                recipe.runtime.generic_input_types = layout.runtime.generic_input_types;
                recipe.runtime.previous_stage_stores = layout.runtime.previous_stage_stores;
                add(std::move(recipe), TC::RecipeSource::InputLayout);
            }
        }
    }
    return selected;
}

void WriteReport(const std::filesystem::path& path, const Summary& summary) {
    Json sources = Json::object();
    constexpr std::array names{"exact_stage", "class", "baseline", "partner_stage", "input_layout"};
    for (size_t i = 0; i < names.size(); ++i)
        sources[names[i]] = {{"modules", summary.sources[i].modules},
                            {"module_bytes", summary.sources[i].module_bytes}};
    Json histogram = Json::object();
    for (const auto& [modules, entries] : summary.multiplicity)
        histogram[std::to_string(modules)] = entries;
    const Json report{{"schema_version", 1}, {"seen", summary.seen},
        {"entries", summary.entries}, {"translated", summary.translated},
        {"validated_modules", summary.validated_modules},
        {"invalidated_modules", summary.invalidated_modules},
        {"replaced_modules", summary.replaced_modules},
        {"resumed", summary.resumed}, {"modules", summary.modules},
        {"module_bytes", summary.module_bytes}, {"module_bytes_encoding", "uncompressed_spirv"},
        {"cancelled", summary.cancelled},
        {"skips", summary.skipped}, {"sources", sources},
        {"product_entry_multiplicity", histogram}, {"product_modules", summary.product_modules},
        {"import", {{"entries", summary.import_entries},
                    {"truncated_entries", summary.import_truncated_entries},
                    {"skipped_records", summary.import_skipped_records},
                    {"bytes", summary.import_bytes},
                    {"scanned_records", summary.import_scanned_records},
                    {"eligible_records", summary.import_eligible_records},
                    {"admitted_records", summary.import_admitted_records},
                    {"framing_skipped_bytes", summary.import_framing_skipped_bytes},
                    {"invalid_recipes", summary.import_invalid_recipes}}}};
    std::ofstream file(path, std::ios::trunc);
    file << report.dump(2) << '\n';
    file.close();
    if (!file) throw std::runtime_error("Cannot write shader report");
}

Input ParseInput(std::string_view text, const std::filesystem::path& directory) {
    if (text.size() > MaxIndexBytes) throw std::runtime_error("index too large");
    const auto json = Json::parse(text);
    if (Unsigned(json.at("schema_version")) != 1) throw std::runtime_error("Unknown input version");
    Input result{.title_id = Hex(json.at("title_id"))};
    const auto& rows = json.at("programs");
    if (!rows.is_array() || rows.size() > 100000) throw std::runtime_error("Invalid program list");
    for (const auto& row : rows) {
        Program program;
        if (row.contains("origins")) {
            const auto& origins = row.at("origins");
            if (!origins.is_array() || origins.size() > 100000)
                throw std::runtime_error("Invalid origin list");
            for (const auto& origin : origins)
                program.origins.push_back({origin.at("source").get<std::string>(),
                    origin.at("member").get<std::string>(), SmallUnsigned(origin.at("variation"))});
        }
        program.container = directory / row.at("container").get<std::string>();
        program.control = directory / row.at("control").get<std::string>();
        program.offset = Unsigned(row.at("offset"));
        program.size = Unsigned(row.at("size"));
        program.control_offset = Unsigned(row.at("control_offset"));
        program.constants_offset = SmallUnsigned(row.at("constants_offset"));
        program.constants_size = SmallUnsigned(row.at("constants_size"));
        program.fingerprint = Hex(row.at("fingerprint"));
        program.stage = Stage(row.at("stage").get<std::string>());
        if (row.contains("tier")) program.tier = SmallUnsigned(row.at("tier"));
        if (program.size == 0 || program.size > MaxProgramBytes ||
            program.offset > UINT64_MAX - program.size ||
            program.control_offset > UINT64_MAX - 0x58)
            throw std::runtime_error("invalid_range");
        const bool graphics = program.stage == Shader::Stage::VertexB ||
                              program.stage == Shader::Stage::Fragment ||
                              program.stage == Shader::Stage::Geometry;
        if (graphics && program.constants_size != 0 &&
            (program.constants_offset > program.size ||
             program.constants_size > program.size - program.constants_offset))
            throw std::runtime_error("invalid_range");
        // A well-formed row whose constant section exceeds what the environment can read exactly
        // is skipped and counted by the worker; it must not reject the rest of the index.
        if (graphics && program.constants_size > MaxBank1Bytes)
            program.skip_reason = "bank1_too_large";
        if (row.contains("bank1_words") && program.skip_reason.empty()) {
            const auto& words = row.at("bank1_words");
            if (!words.is_array() || words.size() > 64 || words.size() * 4 != program.constants_size)
                throw std::runtime_error("constants_mismatch");
            for (const auto& word : words) program.bank1_words.push_back(SmallUnsigned(word));
        }
        result.programs.push_back(std::move(program));
    }
    std::stable_sort(result.programs.begin(), result.programs.end(),
                     [](const auto& a, const auto& b) { return a.tier < b.tier; });
    return result;
}

Summary Run(const std::filesystem::path& input, u64 title_id, u32 workers,
            const std::filesystem::path& recipe_cache, const Shader::Profile& profile,
            const Shader::HostTranslateInfo& host, const TranslationCache::Digest& config,
            TranslationCache::Store& store, u32 recipe_count, u32 recipe_limit,
            const std::filesystem::path& report, TranslationCache::RecipeBaseline baseline) {
    namespace TC = TranslationCache;
    const auto begin = std::chrono::steady_clock::now();
    const auto index_size = std::filesystem::file_size(input);
    if (index_size > MaxIndexBytes)
        throw std::runtime_error(fmt::format("index too large: {} bytes exceeds the {} MiB limit",
                                             index_size, MaxIndexBytes >> 20));
    std::vector<u8> bytes(index_size);
    {
        std::ifstream file(input, std::ios::binary);
        if (!file || !file.read(reinterpret_cast<char*>(bytes.data()),
                                static_cast<std::streamsize>(bytes.size())))
            throw std::runtime_error("input_io: cannot read the index");
    }
    const auto parsed = ParseInput({reinterpret_cast<const char*>(bytes.data()), bytes.size()},
                                   input.parent_path());
    if (parsed.title_id != title_id) throw std::runtime_error("Archive title ID differs from game");
    if (!store.Enabled()) throw std::runtime_error("Translation cache must be enabled");
    if (recipe_count == 0 || recipe_count > 64)
        throw std::runtime_error("Recipe count must be between 1 and 64");
    TC::RecipeCatalog recipes;
    if (!recipe_cache.empty()) recipes = TC::Store::ReadRecipeCatalog(recipe_cache, config, recipe_limit);
    if (workers == 0) workers = std::max(1U, std::thread::hardware_concurrency() / 2);
    workers = std::min({workers, 64U, std::max(1U, static_cast<u32>(parsed.programs.size()))});
    Summary summary;
    summary.seen = parsed.programs.size();
    summary.import_entries = recipes.entries;
    summary.import_truncated_entries = recipes.truncated_entries;
    summary.import_skipped_records = recipes.skipped_records;
    summary.import_bytes = recipes.imported_bytes;
    summary.import_scanned_records = recipes.scanned_records;
    summary.import_eligible_records = recipes.eligible_records;
    summary.import_admitted_records = recipes.admitted_records;
    summary.import_framing_skipped_bytes = recipes.framing_skipped_bytes;
    summary.import_invalid_recipes = recipes.invalid_recipes;
    auto environment = [](const Program& row) {
        const auto program_bytes = Read(row.container, row.offset, row.size);
        const auto control = Read(row.control, row.control_offset, 0x58);
        u64 fingerprint = 0xcbf29ce484222325ULL;
        for (const auto byte : std::span(program_bytes).first(std::min<size_t>(256, row.size)))
            fingerprint = (fingerprint ^ byte) * 0x100000001b3ULL;
        if (fingerprint != row.fingerprint) throw std::runtime_error("fingerprint_mismatch");
        if (Word(control, 0x40) != row.constants_offset || Word(control, 0x3c) != row.constants_size)
            throw std::runtime_error("constants_mismatch");
        for (size_t i = 0; i < row.bank1_words.size(); ++i)
            if (Word(program_bytes, row.constants_offset + i * 4) != row.bank1_words[i])
                throw std::runtime_error("constants_mismatch");
        return ArchiveEnvironment(program_bytes, control, row.stage);
    };
    using Member = std::pair<std::string, std::string>;
    std::map<Member, std::vector<std::pair<u32, size_t>>> members;
    std::vector<std::optional<PartnerStage>> partner_outputs(parsed.programs.size());
    std::vector<Shader::VaryingState> observed_inputs(parsed.programs.size());
    std::vector<std::vector<const TC::EmissionRecipe*>> recorded_layouts(parsed.programs.size());
    for (size_t i = 0; i < parsed.programs.size(); ++i) {
        const auto& row = parsed.programs[i];
        if (row.stage == Shader::Stage::VertexB)
            for (const auto& origin : row.origins)
                members[{origin.source, origin.member}].emplace_back(origin.variation, i);
    }
    // Prepare each vertex once. EmitSPIRV determines descriptor allocation, including profile-
    // dependent bindings; counting descriptors by hand would disagree with the runtime emitter.
    std::atomic<size_t> prepare_next{};
    std::atomic<u64> partner_translated{};
    auto prepare = [&] {
        while (!StopSignal()) {
            const size_t i = prepare_next.fetch_add(1);
            if (i >= parsed.programs.size()) break;
            const auto& row = parsed.programs[i];
            if (row.stage != Shader::Stage::VertexB || row.origins.empty()) continue;
            try {
                if (!row.skip_reason.empty()) continue;
                auto env = environment(row);
                const auto hash = env.Analyze();
                if (!hash) continue;
                const auto key = TC::MakeStageKey(env, *hash, static_cast<u32>(row.stage) + 1,
                                                 recipes.source_config);
                if (const auto it = recipes.exact.find(key); it != recipes.exact.end())
                    for (const auto& recipe : it->second) recorded_layouts[i].push_back(&recipe.recipe);
                TC::RecordingEnvironment recording(env, true);
                Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_pool;
                Shader::ObjectPool<Shader::IR::Inst> inst_pool;
                Shader::ObjectPool<Shader::IR::Block> block_pool;
                Shader::Maxwell::Flow::CFG cfg(recording, flow_pool,
                    Shader::Maxwell::Location{static_cast<u32>(env.StartAddress() + sizeof(Shader::ProgramHeader))});
                auto program = Shader::Maxwell::TranslateProgram(inst_pool, block_pool, recording, cfg, host);
                ++partner_translated;
                if (!env.CanCacheTranslation() || program.info.requires_layer_emulation) continue;
                observed_inputs[i].mask = program.info.loads.mask | program.info.passthrough.mask;
                auto recipe = TC::DefaultEmissionRecipe(row.stage);
                recipe.runtime = TC::CanonicalRuntimeInfo(recipe.runtime, program.info, program.stage);
                Shader::Maxwell::ConvertLegacyToGeneric(program, recipe.runtime);
                auto bindings = recipe.bindings;
                const auto code = Shader::Backend::SPIRV::EmitSPIRV(profile, recipe.runtime, program, bindings);
                if (code.size() >= 5 && code.front() == 0x07230203)
                    partner_outputs[i] = PartnerStage{bindings, program.info};
            } catch (const std::exception& error) {
                LOG_DEBUG(Render_Vulkan, "Offline partner {}: {}", i, error.what());
            }
        }
    };
    {
        std::vector<std::jthread> threads;
        for (u32 i = 0; i < workers; ++i) threads.emplace_back(prepare);
    }
    summary.translated = partner_translated.load();
    auto select = [&](size_t index, const TC::Digest& key, const Shader::Info& info) {
        const auto& row = parsed.programs[index];
        std::vector<PartnerStage> partners;
        std::vector<TC::EmissionRecipe> layouts;
        std::unordered_set<size_t> seen;
        std::unordered_set<TC::Digest, TC::DigestHash> seen_layouts, seen_partners;
        // Prefer the exact variation across all aliases before trying other variations.
        for (const bool same : {true, false}) {
            for (const auto& origin : row.origins) {
                const auto it = members.find({origin.source, origin.member});
                if (it == members.end()) continue;
                for (const auto& [variation, vertex] : it->second) {
                    if ((variation == origin.variation) != same || !seen.insert(vertex).second) continue;
                    if (row.stage == Shader::Stage::VertexB &&
                        (vertex == index || CanShareInputLayout(info, observed_inputs[vertex]))) {
                        for (const auto* recorded : recorded_layouts[vertex]) {
                            TC::EmissionRecipe layout;
                            layout.runtime.generic_input_types = recorded->runtime.generic_input_types;
                            layout.runtime.previous_stage_stores = recorded->runtime.previous_stage_stores;
                            const auto digest = TC::DigestRuntimeInfo(
                                TC::CanonicalRuntimeInfo(layout.runtime, info, row.stage));
                            if (seen_layouts.insert(digest).second) layouts.push_back(std::move(layout));
                        }
                    }
                    if (row.stage == Shader::Stage::Fragment && partner_outputs[vertex] &&
                        partners.size() < recipe_count) {
                        const auto& partner = *partner_outputs[vertex];
                        Shader::RuntimeInfo runtime;
                        runtime.previous_stage_stores = partner.info.stores;
                        runtime.previous_stage_legacy_stores_mapping = partner.info.legacy_stores_mapping;
                        const auto digest = TC::MakeVariantKey({}, TC::DigestRuntimeInfo(
                            TC::CanonicalRuntimeInfo(runtime, info, row.stage)), partner.bindings);
                        if (seen_partners.insert(digest).second) partners.push_back(partner);
                    }
                }
            }
        }
        return ExpandRecipes(recipes.SelectWithSources(key, row.stage, info, recipe_count, baseline),
                             row.stage, info, partners, layouts);
    };
    std::mutex results;
    // Serialize duplicate stage candidates through lookup and append, without serializing all work.
    std::array<std::mutex, 64> stage_locks;
    std::array<std::unordered_set<TC::Digest, TC::DigestHash>, 64> damaged;
    std::atomic<size_t> next{};
    auto work = [&] {
        while (!StopSignal()) {
            const size_t index = next.fetch_add(1);
            if (index >= parsed.programs.size()) break;
            const auto& row = parsed.programs[index];
            try {
                if (row.stage != Shader::Stage::VertexB && row.stage != Shader::Stage::Fragment &&
                    row.stage != Shader::Stage::Geometry) throw std::runtime_error("unsupported_stage");
                if (!row.skip_reason.empty()) throw std::runtime_error(row.skip_reason);
                auto env = environment(row);
                const auto hash = env.Analyze();
                if (!hash) throw std::runtime_error("unsupported_lowering");
                const auto key = TC::MakeStageKey(env, *hash, static_cast<u32>(row.stage) + 1, config);
                const auto recipe_key = recipe_cache.empty() ? key :
                    TC::MakeStageKey(env, *hash, static_cast<u32>(row.stage) + 1, recipes.source_config);
                std::scoped_lock stage_lock(stage_locks[key.lo % stage_locks.size()]);
                auto& damaged_variants = damaged[key.lo % stage_locks.size()];
                auto ready = [&](const TC::Digest& variant_key) {
                    const auto state = store.ValidateVariantForResume(variant_key);
                    std::scoped_lock lock(results);
                    if (state == TC::Store::ResumeVariant::Validated) ++summary.validated_modules;
                    if (state == TC::Store::ResumeVariant::Invalidated) {
                        ++summary.invalidated_modules;
                        damaged_variants.insert(variant_key);
                    }
                    return state == TC::Store::ResumeVariant::Ready ||
                           state == TC::Store::ResumeVariant::Validated;
                };
                if (StopSignal()) break;
                TC::RecordingEnvironment resume(env, true);
                auto entry = store.FindEntry(key, resume);
                std::vector<TC::SelectedRecipe> selected;
                if (entry)
                    selected = select(index, recipe_key, entry->meta.info);
                size_t candidate{};
                bool translated{};
                while (!StopSignal()) {
                    if (entry) {
                        while (candidate < selected.size()) {
                            const auto& recipe = selected[candidate].recipe;
                            const auto runtime = TC::CanonicalRuntimeInfo(recipe.runtime,
                                entry->meta.info, entry->meta.stage);
                            const auto variant_key = TC::MakeVariantKey(entry->id,
                                TC::DigestRuntimeInfo(runtime), recipe.bindings);
                            if (!ready(variant_key)) break;
                            ++candidate;
                        }
                        if (candidate == selected.size()) break;
                    }
                    // Each emission owns a fresh program: EmitSPIRV and legacy conversion mutate IR.
                    TC::RecordingEnvironment recording(env, true);
                    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_pool;
                    Shader::ObjectPool<Shader::IR::Inst> inst_pool;
                    Shader::ObjectPool<Shader::IR::Block> block_pool;
                    Shader::Maxwell::Flow::CFG cfg(recording, flow_pool,
                        Shader::Maxwell::Location{static_cast<u32>(env.StartAddress() + sizeof(Shader::ProgramHeader))});
                    auto program = Shader::Maxwell::TranslateProgram(inst_pool, block_pool, recording, cfg, host);
                    if (!env.CanCacheTranslation() || program.info.requires_layer_emulation)
                        throw std::runtime_error("lowering_or_emission_rejected");
                    translated = true;
                    {
                        std::scoped_lock lock(results);
                        ++summary.translated;
                    }
                    if (!entry) {
                        entry = store.Insert(key, recording.TakeQueries(), TC::ProgramMeta::From(program), true);
                        if (!entry) throw std::runtime_error("cache_disabled");
                        selected = select(index, recipe_key, program.info);
                    }
                    if (selected.empty()) break;
                    const auto selection = selected[candidate++];
                    auto recipe = selection.recipe;
                    recipe.runtime = TC::CanonicalRuntimeInfo(recipe.runtime, program.info, program.stage);
                    const auto variant_key = TC::MakeVariantKey(entry->id,
                        TC::DigestRuntimeInfo(recipe.runtime), recipe.bindings);
                    if (ready(variant_key)) continue;
                    Shader::Maxwell::ConvertLegacyToGeneric(program, recipe.runtime);
                    auto bindings = recipe.bindings;
                    const auto code = Shader::Backend::SPIRV::EmitSPIRV(profile, recipe.runtime, program, bindings);
                    if (code.size() < 5 || code.front() != 0x07230203)
                        throw std::runtime_error("lowering_or_emission_rejected");
                    if (!store.InsertVariant(*entry, variant_key,
                                             {bindings, program.info, code, true}, &recipe))
                        throw std::runtime_error("cache_disabled");
                    std::scoped_lock lock(results);
                    auto& source = summary.sources[static_cast<size_t>(selection.source)];
                    ++source.modules;
                    source.module_bytes += code.size() * sizeof(u32);
                    ++summary.modules;
                    if (damaged_variants.erase(variant_key)) ++summary.replaced_modules;
                    summary.module_bytes += code.size() * sizeof(u32);
                }
                if (!translated && !StopSignal()) {
                    std::scoped_lock lock(results);
                    ++summary.resumed;
                }
            } catch (const std::exception& error) {
                std::scoped_lock lock(results);
                ++summary.skipped[Reason(error.what())];
                LOG_DEBUG(Render_Vulkan, "Offline translation: program {}: {}", index, error.what());
            }
        }
    };
    {
        std::vector<std::jthread> threads;
        for (u32 i = 0; i < workers; ++i) threads.emplace_back(work);
    }
    summary.multiplicity = store.VariantMultiplicity();
    for (const auto& [modules, entries] : summary.multiplicity) {
        summary.entries += entries;
        summary.product_modules += modules * entries;
    }
    summary.cancelled = StopSignal() != 0;
    constexpr std::array names{"exact_stage", "class", "baseline", "partner_stage", "input_layout"};
    for (size_t i = 0; i < names.size(); ++i)
        LOG_INFO(Render_Vulkan, "Offline translation: source={} modules={} module_bytes={}",
                 names[i], summary.sources[i].modules, summary.sources[i].module_bytes);
    for (const auto& [modules, entries] : summary.multiplicity)
        LOG_INFO(Render_Vulkan, "Offline translation: multiplicity={} entries={}", modules, entries);
    LOG_INFO(Render_Vulkan, "Offline translation: product_entries={} product_modules={}",
             summary.entries, summary.product_modules);
    if (summary.cancelled) {
        LOG_INFO(Render_Vulkan, "Offline translation: stopped at record boundary signal={}", StopSignal());
    }
    u64 skipped{};
    for (const auto& [reason, count] : summary.skipped) {
        skipped += count;
        LOG_INFO(Render_Vulkan, "Offline translation: skipped reason={} count={}", reason, count);
    }
    LOG_INFO(Render_Vulkan, "Offline translation: seen={} entries={} translated={} resumed={} validated_modules={} invalidated_modules={} replaced_modules={} skipped={} modules={} module_bytes={} elapsed_ms={}",
             summary.seen, summary.entries, summary.translated, summary.resumed, summary.validated_modules, summary.invalidated_modules, summary.replaced_modules, skipped, summary.modules, summary.module_bytes,
             std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count());
    if (!report.empty()) WriteReport(report, summary);
    return summary;
}
} // namespace VideoCommon::OfflineShaders
