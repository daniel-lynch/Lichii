// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
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
    explicit ComputeEnvironment(bool broken = false, Shader::Stage shader_stage = Shader::Stage::Compute) {
        stage = shader_stage;
        code = {0, 0xe30000000007000fULL, 0, 0};
        cached_lowest = stage == Shader::Stage::Compute ? 0 : sizeof(Shader::ProgramHeader);
        cached_highest = cached_lowest + 24;
        mesh_registers = {4 | (3 << 4) | (1 << 13), 1 << 20, 1};
        start_address = broken ? 0x100 : 0;
        if (stage == Shader::Stage::Compute) workgroup_size = {1, 1, 1};
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
    const auto result = VideoCommon::OfflineShaders::ProduceRecorded(source, profile, {}, config, store);
    REQUIRE(result.failures.empty());
    CHECK(result.records == 1);
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
    const auto resumed = VideoCommon::OfflineShaders::ProduceRecorded(source, profile, {}, config, store);
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
