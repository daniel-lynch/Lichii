// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <bit>
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <catch2/catch_test_macros.hpp>
#include "common/logging.h"
#include "video_core/offline_recorded_producer.h"
#include "video_core/pipeline_cache_files.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/shader_environment.h"

namespace {
struct TempDir {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("eden-recorded-producer-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    TempDir() { std::filesystem::create_directories(path); }
    ~TempDir() { std::filesystem::remove_all(path); }
};
std::string ReadAll(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
class ComputeEnvironment final : public VideoCommon::GenericEnvironment {
public:
    explicit ComputeEnvironment(bool broken = false, Shader::Stage shader_stage = Shader::Stage::Compute,
                                std::optional<Shader::TextureType> texture = std::nullopt) {
        stage = shader_stage;
        code = {0, 0xe30000000007000fULL, 0, 0};
        if (texture) {
            // TXQ dimension R0, RZ, c[0][0], mask X; EXIT. The fragment exports R0.
            code = {0, 0xdf4800008047ff00ULL, 0, 0xe30000000007000fULL};
            sph.ps.omap.target = 1;
            cbuf_values.emplace(0, 7);
            texture_types.emplace(7, *texture);
            texture_pixel_formats.emplace(7, Shader::TexturePixelFormat::A8B8G8R8_UNORM);
        }
        cached_lowest = stage == Shader::Stage::Compute ? 0 : sizeof(Shader::ProgramHeader);
        cached_highest = cached_lowest + 24;
        mesh_registers = {4 | (3 << 4) | (1 << 13), 1 << 20, 1};
        start_address = broken ? 0x100 : 0;
        if (stage == Shader::Stage::Compute) workgroup_size = {1, 1, 1};
    }
    void UseUniformVertex() {
        // LDC.32 R0, c[0][RZ]; AST generic0.x, R0; EXIT.
        code = {0, 0xef9400000007ff00ULL, 0, 0xeff07f800807ff00ULL, 0xe30000000007000fULL};
        cached_highest = cached_lowest + 32;
    }
    u32 ReadCbufValue(u32, u32) override { return 0; }
    Shader::TextureType ReadTextureType(u32) override { return Shader::TextureType::Color2D; }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override { return {}; }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 1; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
};
}

TEST_CASE("Recorded producer validates every record on resume without changing sources",
          "[video_core][offline_recorded_producer]") {
    namespace TC = VideoCommon::TranslationCache;
    TempDir dir;
    const auto source = dir.path / "pipelines.bin";
    Vulkan::ComputePipelineCacheKey key{0x12345678, 0, {1, 1, 1}};
    ComputeEnvironment good;
    ComputeEnvironment broken(true);
    for (auto* env : {&good, &good, &broken}) {
        const std::array<const VideoCommon::GenericEnvironment*, 1> envs{env};
        VideoCommon::SerializePipeline(key, envs, source, VideoCommon::VulkanPipelineCacheVersion);
    }
    const auto original = ReadAll(source);
    Shader::Profile profile{};
    profile.supported_spirv = 0x00010300;
    const auto config = TC::MakeConfigDigest(profile, {}, 0);
    TC::Store store;
    store.Open(dir.path / "translations.bin", config);
    const auto first = VideoCommon::OfflineShaders::ProduceRecorded(source, profile, {}, config, store);
    REQUIRE(first.records == 3);
    REQUIRE(first.stages == 2);
    REQUIRE(first.failed_records == 1);
    REQUIRE(first.failures.size() == 1);
    CHECK(first.failures.front().record == 3);
    CHECK(first.failures.front().pipeline == key.Hash());
    CHECK(first.failures.front().slot == 5);
    CHECK(first.failures.front().shader == key.unique_hash);
    CHECK(first.entries == 1);
    CHECK(first.modules == 1);
    const auto resumed = VideoCommon::OfflineShaders::ProduceRecorded(source, profile, {}, config, store);
    CHECK(resumed.records == 3);
    CHECK(resumed.stages == 2);
    CHECK(resumed.failed_records == 1);
    CHECK(resumed.entries == 0);
    CHECK(resumed.modules == 0);
    CHECK(resumed.resumed == 2);
    CHECK(ReadAll(source) == original);
}

TEST_CASE("Recorded producer preserves corrupt and incompatible input files",
          "[video_core][offline_recorded_producer]") {
    TempDir dir;
    const auto source = dir.path / "pipelines.bin";
    SECTION("short header") { std::ofstream(source, std::ios::binary) << "yuzucach"; }
    SECTION("old version") {
        std::ofstream file(source, std::ios::binary);
        file << "yuzucach";
        const u32 version = VideoCommon::VulkanPipelineCacheVersion - 1;
        file.write(reinterpret_cast<const char*>(&version), sizeof(version));
    }
    SECTION("invalid environment count") {
        std::ofstream file(source, std::ios::binary);
        file << "yuzucach";
        const u32 version = VideoCommon::VulkanPipelineCacheVersion;
        const u32 count = 1000;
        file.write(reinterpret_cast<const char*>(&version), sizeof(version));
        file.write(reinterpret_cast<const char*>(&count), sizeof(count));
    }
    const auto original = ReadAll(source);
    VideoCommon::TranslationCache::Store store;
    const auto result = VideoCommon::OfflineShaders::ProduceRecorded(source, {}, {}, {}, store);
    REQUIRE(result.failures.size() == 1);
    CHECK(result.records == 0);
    CHECK(ReadAll(source) == original);
}

TEST_CASE("Recorded task and mesh entries use the pipeline slots and replay mesh state",
          "[video_core][offline_recorded_producer]") {
    namespace TC = VideoCommon::TranslationCache;
    VideoCommon::OfflineShaders::RecordedOptions options;
    SECTION("existing recipe selection") {}
    SECTION("exact key recipes") { options.exact_pipeline_recipes = true; }
    TempDir dir;
    const auto source = dir.path / "mesh.bin";
    ComputeEnvironment task(false, Shader::Stage::Task);
    ComputeEnvironment mesh(false, Shader::Stage::Mesh);
    Vulkan::GraphicsPipelineCacheKey key{};
    key.unique_hashes[1] = 0x123;
    key.unique_hashes[2] = 0x456;
    const std::array<const VideoCommon::GenericEnvironment*, 2> envs{&task, &mesh};
    VideoCommon::SerializePipeline(key, envs, source, VideoCommon::VulkanPipelineCacheVersion);
    Shader::Profile profile{};
    profile.supported_spirv = 0x00010500;
    const auto config = TC::MakeConfigDigest(profile, {}, 0);
    TC::Store store;
    store.Open(dir.path / "translations.bin", config);
    const auto result = VideoCommon::OfflineShaders::ProduceRecorded(source, profile, {}, config, store, nullptr, 0, options);
    REQUIRE(result.failures.empty());
    CHECK(result.records == 1);
    if (options.exact_pipeline_recipes) {
        REQUIRE(result.lookups.size() == 1);
        REQUIRE(result.lookups[0].stages.size() == 2);
        CHECK(result.lookups[0].stages[0].hit);
        CHECK(result.lookups[0].stages[1].hit);
    }
    CHECK(result.stages == 2);
    CHECK(result.entries == 2);
    CHECK(result.modules == 2);
    CHECK(store.HasStageKey(TC::MakeStageKey(task, 0x123, 1, config)));
    CHECK(store.HasStageKey(TC::MakeStageKey(mesh, 0x456, 2, config)));
    CHECK_FALSE(store.HasStageKey(TC::MakeStageKey(mesh, 0x456, 1, config)));
    CHECK(result.stages_by_type[static_cast<size_t>(Shader::Stage::Task)] == 1);
    CHECK(result.stages_by_type[static_cast<size_t>(Shader::Stage::Mesh)] == 1);
    CHECK(std::ranges::all_of(result.uncovered_by_type, [](u64 count) { return count == 0; }));
    store.Close();
    store.Open(dir.path / "translations.bin", config);
    const auto resumed = VideoCommon::OfflineShaders::ProduceRecorded(source, profile, {}, config, store, nullptr, 0, options);
    REQUIRE(resumed.failures.empty());
    CHECK(resumed.modules == 0);
    CHECK(resumed.resumed == 2);
    const auto catalog = TC::Store::ReadRecipeCatalog(dir.path / "translations.bin", config);
    CHECK(catalog.invalid_recipes == 0);
    CHECK(catalog.exact.empty()); // Offline baselines are not observed runtime recipes.
    const auto runtime_path = dir.path / "runtime.bin";
    {
        TC::Store runtime;
        runtime.Open(runtime_path, config);
        for (const auto stage : {Shader::Stage::Task, Shader::Stage::Mesh}) {
            const auto entry = runtime.Insert({static_cast<u64>(stage), 0}, {}, {.stage = stage});
            const auto recipe = TC::DefaultEmissionRecipe(stage);
            REQUIRE(runtime.InsertVariant(*entry,
                TC::MakeVariantKey(entry->id, TC::DigestRuntimeInfo(recipe.runtime), recipe.bindings),
                {.spirv = {0x07230203, 0x10500, 0, 1, 0}}, &recipe));
        }
    }
    const auto observed = TC::Store::ReadRecipeCatalog(runtime_path, config);
    CHECK(observed.invalid_recipes == 0);
    CHECK(observed.exact.size() == 2);
}

TEST_CASE("Offline producer validates explicit private inputs without a window",
          "[.][offline_producer_validation]") {
    namespace Offline = VideoCommon::OfflineShaders;
    namespace TC = VideoCommon::TranslationCache;
    const char* index = std::getenv("EDEN_OFFLINE_VALIDATION_INDEX");
    const char* recipes = std::getenv("EDEN_OFFLINE_VALIDATION_RECIPES");
    const char* report = std::getenv("EDEN_OFFLINE_VALIDATION_REPORT");
    const char* cache = std::getenv("EDEN_OFFLINE_VALIDATION_CACHE");
    const char* title = std::getenv("EDEN_OFFLINE_VALIDATION_TITLE");
    REQUIRE(index);
    REQUIRE(recipes);
    REQUIRE(report);
    REQUIRE(cache);
    REQUIRE(title);
    Common::Log::Initialize();
    // A portable emission profile; this is not device-profile validation.
    Shader::Profile profile{};
    profile.supported_spirv = 0x00010500;
    profile.support_int64 = true;
    Shader::HostTranslateInfo host{};
    host.support_int64 = true;
    host.ApplyDescriptorLimitPolicy();
    const auto config = TC::MakeConfigDigest(profile, host, 0);
    TC::Store store;
    std::filesystem::create_directories(std::filesystem::path(cache).parent_path());
    store.Open(cache, config);
    const auto summary = Offline::Run(index, std::stoull(title, nullptr, 16), 8,
                                     recipes, profile, host, config, store,
                                     TC::DefaultRecipeCount, TC::DefaultRecipeLimit, report);
    REQUIRE_FALSE(summary.cancelled);
    REQUIRE(store.Persistent());
    REQUIRE(summary.seen > 0);
}

namespace {
std::vector<VideoCommon::FileEnvironment> ReadGraphicsRecord(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    file.seekg(12);
    u32 count{};
    file.read(reinterpret_cast<char*>(&count), sizeof(count));
    std::vector<VideoCommon::FileEnvironment> envs(count);
    for (auto& env : envs) env.Deserialize(file);
    return envs;
}
}

TEST_CASE("Exact recorded recipes close runtime variant misses and validate after reopen",
          "[video_core][offline_recorded_producer]") {
    namespace TC = VideoCommon::TranslationCache;
    namespace Offline = VideoCommon::OfflineShaders;
    TempDir dir;
    const auto source = dir.path / "graphics.bin";
    ComputeEnvironment vertex(false, Shader::Stage::VertexB);
    ComputeEnvironment fragment(false, Shader::Stage::Fragment);
    vertex.UseUniformVertex();
    Vulkan::GraphicsPipelineCacheKey key{};
    key.unique_hashes[1] = 0x123;
    key.unique_hashes[5] = 0x456;
    key.state.y_negate.Assign(1);
    key.state.point_size = std::bit_cast<u32>(1.0f);
    const std::array<const VideoCommon::GenericEnvironment*, 2> envs{&vertex, &fragment};
    VideoCommon::SerializePipeline(key, envs, source, VideoCommon::VulkanPipelineCacheVersion);
    const auto original = ReadAll(source);
    Shader::Profile profile{};
    profile.supported_spirv = 0x00010500;
    const auto config = TC::MakeConfigDigest(profile, {}, 0);
    TC::Store store;
    store.Open(dir.path / "translations.bin", config);
    const auto baseline = Offline::ProduceRecorded(source, profile, {}, config, store);
    REQUIRE(baseline.failures.empty());
    auto recorded = ReadGraphicsRecord(source);
    const auto before = Offline::ValidateRecordedGraphics(key, recorded, profile, {}, config, store);
    REQUIRE(before.stages.size() == 2);
    CHECK_FALSE(before.stages[0].hit);
    CHECK(before.stages[0].reason == "variant_miss");
    const auto result = Offline::ProduceRecorded(source, profile, {}, config, store, nullptr, 0, {true});
    REQUIRE(result.failures.empty());
    REQUIRE(result.lookups.size() == 1);
    CHECK(result.validation_reloaded);
    CHECK(result.modules == 2);
    CHECK(std::ranges::all_of(result.lookups[0].stages, &Offline::RecordedStageLookup::hit));
    const auto vertex_variant = store.FindVariant(result.lookups[0].stages[0].variant);
    REQUIRE(vertex_variant);
    CHECK(vertex_variant->binding_out.uniform_buffer > 0);
    const auto fragment_entry = store.FindEntry(TC::MakeStageKey(recorded[1], 0x456, 5, config), recorded[1]);
    REQUIRE(fragment_entry);
    std::array<Shader::IR::Program, 6> metadata;
    metadata[1].stage = Shader::Stage::VertexB;
    metadata[1].info = vertex_variant->info;
    metadata[5] = fragment_entry->meta.ToProgram();
    const auto runtime = TC::CanonicalRuntimeInfo(Vulkan::MakeGraphicsRuntimeInfo(
        metadata, key, metadata[5], &metadata[1], false, false), metadata[5].info, metadata[5].stage);
    CHECK(result.lookups[0].stages[1].variant == TC::MakeVariantKey(fragment_entry->id,
        TC::DigestRuntimeInfo(runtime), vertex_variant->binding_out));
    CHECK_FALSE(store.FindVariant(TC::MakeVariantKey(fragment_entry->id,
        TC::DigestRuntimeInfo(runtime), {})));
    store.Close();
    store.Open(dir.path / "translations.bin", config);
    const auto after = Offline::ValidateRecordedGraphics(key, recorded, profile, {}, config, store);
    INFO(Offline::RecordedReport({.exact_validation = true, .lookups = {after}}));
    CHECK(std::ranges::all_of(after.stages, &Offline::RecordedStageLookup::hit));
    const auto resumed = Offline::ProduceRecorded(source, profile, {}, config, store, nullptr, 0, {true});
    CHECK(resumed.modules == 0);
    CHECK(resumed.resumed == 2);
    CHECK(ReadAll(source) == original);

    auto absent = key;
    absent.unique_hashes[5] = 0x789;
    const auto missing = Offline::ValidateRecordedGraphics(absent, recorded, profile, {}, config, store);
    CHECK(missing.stages[0].hit);
    CHECK_FALSE(missing.stages[1].hit);
    CHECK(missing.stages[1].reason == "entry_miss");
    Offline::RecordedOutcome report;
    report.exact_validation = true;
    report.lookups = {after, missing, before};
    const auto json = nlohmann::json::parse(Offline::RecordedReport(report));
    const auto& exact = json.at("exact_variants");
    CHECK(exact.at("key_hits") == 1);
    CHECK(exact.at("key_misses") == 2);
    CHECK(exact.at("stage_hits") == 3);
    CHECK(exact.at("stage_misses") == 3);
    CHECK(exact.at("keys").at(1).at("stages").at(1).at("reason") == "entry_miss");
}

TEST_CASE("Shared graphics runtime recipes retain mesh primitive outputs and fixed state",
          "[video_core][offline_recorded_producer]") {
    std::array<Shader::IR::Program, 6> programs;
    auto& mesh = programs[2];
    mesh.stage = Shader::Stage::Mesh;
    mesh.mesh.topology = 0;
    mesh.mesh.primitive_lines.push_back(static_cast<u32>(Shader::IR::Attribute::Generic2X));
    mesh.info.stores.Set(Shader::IR::Attribute::Generic2X);
    auto& fragment = programs[5];
    fragment.stage = Shader::Stage::Fragment;
    fragment.info.loads.Set(Shader::IR::Attribute::Generic2X);
    Vulkan::GraphicsPipelineCacheKey key{};
    key.unique_hashes[2] = 1;
    key.unique_hashes[5] = 2;
    key.state.point_size = std::bit_cast<u32>(3.0f);
    key.state.ndc_minus_one_to_one.Assign(1);
    const auto mr = Vulkan::MakeGraphicsRuntimeInfo(programs, key, mesh, nullptr, false, false);
    CHECK(mr.fixed_state_point_size == 3.0f);
    CHECK(mr.convert_depth_mode);
    const auto fr = Vulkan::MakeGraphicsRuntimeInfo(programs, key, fragment, &mesh, false, false);
    CHECK(fr.per_primitive_generics.test(2));
    CHECK(fr.previous_stage_stores[Shader::IR::Attribute::Generic2X]);
    CHECK(VideoCommon::TranslationCache::CanonicalRuntimeInfo(fr, fragment.info, fragment.stage)
              .per_primitive_generics.test(2));
}

TEST_CASE("Exact synthesis retains different texture answers under the same stage key",
          "[video_core][offline_recorded_producer]") {
    namespace TC = VideoCommon::TranslationCache;
    namespace Offline = VideoCommon::OfflineShaders;
    TempDir dir;
    const auto source = dir.path / "textures.bin";
    ComputeEnvironment vertex(false, Shader::Stage::VertexB);
    ComputeEnvironment first(false, Shader::Stage::Fragment, Shader::TextureType::Color2D);
    ComputeEnvironment second(false, Shader::Stage::Fragment, Shader::TextureType::Color3D);
    Vulkan::GraphicsPipelineCacheKey key{};
    key.state.point_size = std::bit_cast<u32>(1.0f);
    key.unique_hashes[1] = 0x123;
    key.unique_hashes[5] = 0x456;
    for (auto* fragment : {&first, &second}) {
        const std::array<const VideoCommon::GenericEnvironment*, 2> envs{&vertex, fragment};
        VideoCommon::SerializePipeline(key, envs, source, VideoCommon::VulkanPipelineCacheVersion);
    }
    Shader::Profile profile{};
    profile.supported_spirv = 0x00010500;
    const auto config = TC::MakeConfigDigest(profile, {}, 0);
    TC::Store store;
    store.Open(dir.path / "translations.bin", config);
    const auto result = Offline::ProduceRecorded(source, profile, {}, config, store, nullptr, 0, {true});
    INFO(Offline::RecordedReport(result));
    REQUIRE(result.failures.empty());
    REQUIRE(result.lookups.size() == 2);
    CHECK(result.entries == 3);
    CHECK(result.modules == 3);
    for (const auto& lookup : result.lookups) {
        REQUIRE(lookup.stages.size() == 2);
        CHECK(lookup.stages[0].hit);
        CHECK(lookup.stages[1].hit);
    }
    CHECK(result.lookups[0].stages[1].stage_key == result.lookups[1].stages[1].stage_key);
    CHECK(result.lookups[0].stages[1].entry != result.lookups[1].stages[1].entry);
    CHECK(result.replay.mismatched == 2);
}

TEST_CASE("Exact validation reports modules rejected by the persisted product reader",
          "[video_core][offline_recorded_producer]") {
    namespace TC = VideoCommon::TranslationCache;
    namespace Offline = VideoCommon::OfflineShaders;
    TempDir dir;
    const auto source = dir.path / "invalid-point.bin";
    ComputeEnvironment vertex(false, Shader::Stage::VertexB);
    Vulkan::GraphicsPipelineCacheKey key{};
    key.unique_hashes[1] = 0x123;
    // A zero point size can be emitted but is rejected by the stored recipe reader.
    const std::array<const VideoCommon::GenericEnvironment*, 1> envs{&vertex};
    VideoCommon::SerializePipeline(key, envs, source, VideoCommon::VulkanPipelineCacheVersion);
    Shader::Profile profile{};
    profile.supported_spirv = 0x00010500;
    const auto config = TC::MakeConfigDigest(profile, {}, 0);
    TC::Store store;
    store.Open(dir.path / "translations.bin", config);
    const auto result = Offline::ProduceRecorded(source, profile, {}, config, store, nullptr, 0, {true});
    REQUIRE(result.validation_reloaded);
    REQUIRE(result.lookups.size() == 1);
    REQUIRE(result.lookups[0].stages.size() == 1);
    CHECK_FALSE(result.lookups[0].stages[0].hit);
    const auto report = nlohmann::json::parse(Offline::RecordedReport(result));
    CHECK(report["exact_variants"]["reloaded_product"] == true);
    CHECK(report["exact_variants"]["key_misses"] == 1);
    CHECK(report["exact_variants"]["stage_misses"] == 1);
}
