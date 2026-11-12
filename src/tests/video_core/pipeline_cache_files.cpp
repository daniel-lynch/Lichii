// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/fs/file_lock.h"
#include "video_core/pipeline_cache_files.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache_records.h"
#include "video_core/shader_environment.h"

namespace {

constexpr std::array<char, 8> Magic{'y', 'u', 'z', 'u', 'c', 'a', 'c', 'h'};
constexpr std::array<char, 8> OtherMagic{'y', 'u', 'z', 'u', 'v', 'k', 'c', 'h'};
/// The EnvironmentReadProperties of the host the tests write records on
constexpr u32 ReadProperties{0x15};

struct TempDir {
    std::filesystem::path path;
    TempDir() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
               ("eden-pipeline-cache-files-" + std::to_string(stamp));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

void WriteCache(const std::filesystem::path& file, const std::array<char, 8>& magic, u32 version,
                std::string_view body) {
    std::ofstream stream(file, std::ios::binary);
    stream.write(magic.data(), magic.size());
    stream.write(reinterpret_cast<const char*>(&version), sizeof(version));
    stream.write(body.data(), static_cast<std::streamsize>(body.size()));
}

std::string ReadAll(const std::filesystem::path& file) {
    std::ifstream stream(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

class SerializedEnvironment final : public VideoCommon::GenericEnvironment {
public:
    explicit SerializedEnvironment(Shader::Stage shader_stage) {
        stage = shader_stage;
        code = {0x123456789abcdef0, 0xfedcba9876543210};
        cached_lowest = 0;
        cached_highest = sizeof(u64);
        mesh_registers = {0x12345678, 0x87654321, 0xabcdef01};
        std::memset(&mesh_primitive_header, 0x5a, sizeof(mesh_primitive_header));
        texture_bound = 2;
        cbuf_values.emplace((u64{1} << 32) | 16, 0x1357);
        texture_types.emplace(3, Shader::TextureType::Color2D);
    }

    /// A constant buffer value, so that records differ
    void AddValue(u32 offset, u32 value) {
        cbuf_values.emplace((u64{2} << 32) | offset, value);
    }
    /// A constant buffer pointer the translation binds
    void AddPointer(u32 offset) {
        storage_pointer_types.emplace((u64{3} << 32) | offset, u8{1});
    }

    u32 ReadCbufValue(u32, u32) override {
        return 0;
    }
    Shader::TextureType ReadTextureType(u32) override {
        return Shader::TextureType::Color2D;
    }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return {};
    }
    bool IsTexturePixelFormatInteger(u32) override {
        return false;
    }
    u32 ReadViewportTransformState() override {
        return 1;
    }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
};

void AppendPipeline(const std::filesystem::path& file, u64 key, Shader::Stage stage) {
    SerializedEnvironment source{stage};
    const std::array<const VideoCommon::GenericEnvironment*, 1> envs{&source};
    VideoCommon::SerializePipeline(key, envs, file, VideoCommon::VulkanPipelineCacheVersion,
                                   ReadProperties);
}

/// The keys of the graphics records a load reaches, and where each record is in the file, found as
/// the Vulkan pipeline cache finds it.
struct LoadedPipelines {
    std::vector<u64> keys;
    std::vector<VideoCommon::PipelineRecord> records;
};

LoadedPipelines LoadKeys(const std::filesystem::path& file) {
    LoadedPipelines loaded;
    u64 begin{Magic.size() + sizeof(u32)};
    VideoCommon::LoadPipelines(
        {}, file, VideoCommon::VulkanPipelineCacheVersion,
        [](std::ifstream&, VideoCommon::FileEnvironment) { FAIL("Unexpected compute record"); },
        [&](std::ifstream& stream, std::vector<VideoCommon::FileEnvironment>) {
            u64 key{};
            stream.read(reinterpret_cast<char*>(&key), sizeof(key));
            loaded.keys.push_back(key);
            loaded.records.push_back(VideoCommon::ReadPipelineRecord(stream, begin));
            begin = loaded.records.back().end;
        });
    return loaded;
}

/// A graphics record keyed as the backends key them, with these environments
void AppendKeyedPipeline(const std::filesystem::path& file, const std::array<u64, 6>& key,
                         std::span<const Shader::Stage> stages) {
    std::vector<std::unique_ptr<SerializedEnvironment>> sources;
    std::vector<const VideoCommon::GenericEnvironment*> envs;
    for (const Shader::Stage stage : stages) {
        envs.push_back(sources.emplace_back(std::make_unique<SerializedEnvironment>(stage)).get());
    }
    VideoCommon::SerializePipeline(key, envs, file, VideoCommon::VulkanPipelineCacheVersion,
                                   ReadProperties);
}

/// The first shader hash of each graphics record a load reaches, checking keys as the backends do
std::vector<u64> LoadCheckedKeys(const std::filesystem::path& file) {
    std::vector<u64> keys;
    VideoCommon::LoadPipelines(
        {}, file, VideoCommon::VulkanPipelineCacheVersion,
        [](std::ifstream&, VideoCommon::FileEnvironment) { FAIL("Unexpected compute record"); },
        [&](std::ifstream& stream, std::vector<VideoCommon::FileEnvironment> envs) {
            std::array<u64, 6> key{};
            stream.read(reinterpret_cast<char*>(key.data()), sizeof(key));
            VideoCommon::CheckGraphicsRecord(key, envs.size());
            keys.push_back(key.front());
        });
    return keys;
}

/// Appends a Fragment record as builds before the read properties wrote it
void AppendOlderPipeline(const std::filesystem::path& file, u64 key) {
    auto newer = file;
    newer += ".newer";
    AppendPipeline(newer, key, Shader::Stage::Fragment);
    std::string record = ReadAll(newer).substr(Magic.size() + sizeof(u32));
    std::filesystem::remove(newer);
    // Without the constant buffer value holding the read properties
    const u64 properties_key{u64{0xffffffff} << 32};
    std::string entry(sizeof(properties_key) + sizeof(ReadProperties), '\0');
    std::memcpy(entry.data(), &properties_key, sizeof(properties_key));
    std::memcpy(entry.data() + sizeof(properties_key), &ReadProperties, sizeof(ReadProperties));
    const size_t at{record.find(entry)};
    REQUIRE(at != std::string::npos);
    record.erase(at, entry.size());
    // The environment count, then the code size and the texture counts before the value count
    const size_t count_at{sizeof(u32) + 3 * sizeof(u64)};
    u64 num_cbuf_values{};
    std::memcpy(&num_cbuf_values, record.data() + count_at, sizeof(num_cbuf_values));
    --num_cbuf_values;
    std::memcpy(record.data() + count_at, &num_cbuf_values, sizeof(num_cbuf_values));
    std::ofstream stream(file, std::ios::binary | std::ios::app);
    stream.write(record.data(), static_cast<std::streamsize>(record.size()));
}

/// A compute translation of one key, binding a pointer or not, made on a host with these read
/// properties
void AppendComputePipeline(const std::filesystem::path& file, u32 value, bool pointer,
                           u32 read_properties = ReadProperties) {
    SerializedEnvironment source{Shader::Stage::Compute};
    source.AddValue(0, value);
    if (pointer) {
        source.AddPointer(0x10);
    }
    const Vulkan::ComputePipelineCacheKey key{
        .unique_hash = 77, .shared_memory_size = 0, .workgroup_size{}};
    const std::array<const VideoCommon::GenericEnvironment*, 1> envs{&source};
    VideoCommon::SerializePipeline(key, envs, file, VideoCommon::VulkanPipelineCacheVersion,
                                   read_properties);
}

/// The compute translations a load reads, with their records, as the Vulkan pipeline cache reads
/// them
std::vector<Vulkan::CachedComputeTranslation> LoadComputeTranslations(
    const std::filesystem::path& file) {
    std::vector<Vulkan::CachedComputeTranslation> translations;
    u64 begin{Magic.size() + sizeof(u32)};
    VideoCommon::LoadPipelines(
        {}, file, VideoCommon::VulkanPipelineCacheVersion,
        [&](std::ifstream& stream, VideoCommon::FileEnvironment env) {
            Vulkan::ComputePipelineCacheKey key{};
            stream.read(reinterpret_cast<char*>(&key), sizeof(key));
            VideoCommon::CheckComputeRecord(env, key.shared_memory_size, key.workgroup_size);
            translations.push_back(
                {std::move(env), VideoCommon::ReadPipelineRecord(stream, begin)});
            begin = translations.back().record.end;
        },
        [](std::ifstream&, std::vector<VideoCommon::FileEnvironment>) {
            FAIL("Unexpected graphics record");
        });
    return translations;
}

/// Lets the translator ask a recorded environment for a value it lacks
void AskMissingValue(VideoCommon::FileEnvironment& env) {
    REQUIRE_THROWS_AS(static_cast<void>(env.ReadCbufValue(9, 0)), Shader::LogicError);
}

} // Anonymous namespace

TEST_CASE("Task and mesh disk environments roundtrip all translation state and record boundaries",
          "[pipeline_cache_files][mesh_serialization]") {
    TempDir temp;
    const auto path = temp.path / "mesh.bin";
    constexpr u64 key = 0xabcdef;
    for (const auto stage : {Shader::Stage::Task, Shader::Stage::Mesh, Shader::Stage::Fragment}) {
        SerializedEnvironment source{stage};
        const std::array<const VideoCommon::GenericEnvironment*, 1> envs{&source};
        VideoCommon::SerializePipeline(key, envs, path, VideoCommon::VulkanPipelineCacheVersion,
                                       ReadProperties);
    }
    size_t read = 0;
    VideoCommon::LoadPipelines(
        {}, path, VideoCommon::VulkanPipelineCacheVersion,
        [](std::ifstream&, VideoCommon::FileEnvironment) { FAIL("Unexpected compute record"); },
        [&](std::ifstream& file, std::vector<VideoCommon::FileEnvironment> envs) {
            const std::array stages{Shader::Stage::Task, Shader::Stage::Mesh,
                                    Shader::Stage::Fragment};
            REQUIRE(read < stages.size());
            REQUIRE(envs.size() == 1);
            auto& restored = envs.front();
            SerializedEnvironment source{stages[read++]};
            REQUIRE(restored.ShaderStage() == source.ShaderStage());
            REQUIRE(restored.ReadInstruction(0) == 0x123456789abcdef0);
            REQUIRE(restored.ReadInstruction(8) == 0xfedcba9876543210);
            REQUIRE(restored.ReadCbufValue(1, 16) == 0x1357);
            REQUIRE(restored.ReadTextureType(3) == Shader::TextureType::Color2D);
            REQUIRE(restored.TextureBoundBuffer() == 2);
            REQUIRE(restored.MemorySize() > sizeof(restored) + 2 * sizeof(u64));
            if (source.ShaderStage() != Shader::Stage::Fragment) {
                REQUIRE(restored.MeshRegisters() == source.MeshRegisters());
                REQUIRE(std::memcmp(&restored.MeshPrimitiveHeader(), &source.MeshPrimitiveHeader(),
                                    sizeof(Shader::ProgramHeader)) == 0);
            }
            u64 restored_key{};
            file.read(reinterpret_cast<char*>(&restored_key), sizeof(restored_key));
            REQUIRE(restored_key == key);
        });
    REQUIRE(read == 3);
}

TEST_CASE("Compatible v52 pipelines migrate without changing or discarding the old warm cache",
          "[pipeline_cache_files][mesh_serialization]") {
    TempDir temp;
    const auto old = VideoCommon::VersionedPipelineCachePath(temp.path, "vulkan", 52);
    const auto current = VideoCommon::VersionedPipelineCachePath(
        temp.path, "vulkan", VideoCommon::VulkanPipelineCacheVersion);
    SerializedEnvironment source{Shader::Stage::Fragment};
    const std::array<const VideoCommon::GenericEnvironment*, 1> envs{&source};
    constexpr u64 key = 42;
    VideoCommon::SerializePipeline(key, envs, old, 52, ReadProperties);
    const auto before = ReadAll(old);
    REQUIRE(VideoCommon::CopyCompatiblePipelineCache(old, current, Magic, 52,
                                                     VideoCommon::VulkanPipelineCacheVersion));
    REQUIRE(ReadAll(old) == before);
    REQUIRE(ReadAll(current).substr(12) == before.substr(12));
    REQUIRE_FALSE(VideoCommon::CopyCompatiblePipelineCache(
        old, current, Magic, 52, VideoCommon::VulkanPipelineCacheVersion));
    bool loaded = false;
    VideoCommon::LoadPipelines(
        {}, current, VideoCommon::VulkanPipelineCacheVersion,
        [](std::ifstream&, VideoCommon::FileEnvironment) { FAIL("Unexpected compute record"); },
        [&](std::ifstream& file, std::vector<VideoCommon::FileEnvironment> restored) {
            REQUIRE(restored.size() == 1);
            REQUIRE(restored.front().ShaderStage() == Shader::Stage::Fragment);
            u64 restored_key{};
            file.read(reinterpret_cast<char*>(&restored_key), sizeof(restored_key));
            REQUIRE(restored_key == key);
            loaded = true;
        });
    REQUIRE(loaded);
    const auto other = temp.path / "wrong.bin";
    REQUIRE_FALSE(VideoCommon::CopyCompatiblePipelineCache(old, other, Magic, 51, 53));
    REQUIRE_FALSE(std::filesystem::exists(other));
}

TEST_CASE("Compatible pipeline cache migration retries after an interrupted temporary copy",
          "[pipeline_cache_files][mesh_serialization][migration_retry]") {
    TempDir temp;
    const auto old = VideoCommon::VersionedPipelineCachePath(temp.path, "vulkan", 52);
    const auto current = VideoCommon::VersionedPipelineCachePath(
        temp.path, "vulkan", VideoCommon::VulkanPipelineCacheVersion);
    auto temporary = current;
    temporary += ".migrating";
    WriteCache(old, Magic, 52, "complete pipeline records");
    const auto before = ReadAll(old);
    {
        std::ofstream stream(temporary, std::ios::binary);
        stream.write(before.data(), 4);
    }
    REQUIRE(ReadAll(temporary) == before.substr(0, 4));
    REQUIRE(VideoCommon::CopyCompatiblePipelineCache(old, current, Magic, 52,
                                                     VideoCommon::VulkanPipelineCacheVersion));
    REQUIRE(ReadAll(old) == before);
    REQUIRE(VideoCommon::PipelineCacheHeaderMatches(
        current, Magic, VideoCommon::VulkanPipelineCacheVersion));
    REQUIRE(ReadAll(current).substr(12) == before.substr(12));
    REQUIRE_FALSE(std::filesystem::exists(temporary));
    const auto migrated = ReadAll(current);
    REQUIRE_FALSE(VideoCommon::CopyCompatiblePipelineCache(
        old, current, Magic, 52, VideoCommon::VulkanPipelineCacheVersion));
    REQUIRE(ReadAll(current) == migrated);
    REQUIRE(ReadAll(old) == before);
}

TEST_CASE("A new cache version starts from the newest compatible cache on disk",
          "[pipeline_cache_files][carry_over]") {
    TempDir temp;
    const auto v52 = VideoCommon::VersionedPipelineCachePath(temp.path, "vulkan", 52);
    const auto v53 = VideoCommon::VersionedPipelineCachePath(temp.path, "vulkan", 53);
    const auto v54 = VideoCommon::VersionedPipelineCachePath(temp.path, "vulkan", 54);
    WriteCache(v52, Magic, 52, "older pipelines");
    WriteCache(v53, Magic, 53, "newer pipelines");
    const auto before_v52 = ReadAll(v52);
    const auto before_v53 = ReadAll(v53);
    constexpr std::array<u32, 2> versions{53, 52};
    const auto carried =
        VideoCommon::CarryOverPipelineCache(temp.path, "vulkan", Magic, versions, 54);
    REQUIRE(carried.version == 53u);
    REQUIRE_FALSE(carried.failed);
    REQUIRE(VideoCommon::PipelineCacheHeaderMatches(v54, Magic, 54));
    REQUIRE(ReadAll(v54).substr(12) == "newer pipelines");
    REQUIRE(ReadAll(v52) == before_v52);
    REQUIRE(ReadAll(v53) == before_v53);
    // A cache of its own version is never replaced, even when an older one changes later.
    WriteCache(v53, Magic, 53, "written by the older build since");
    const auto again =
        VideoCommon::CarryOverPipelineCache(temp.path, "vulkan", Magic, versions, 54);
    REQUIRE_FALSE(again.version);
    REQUIRE_FALSE(again.failed);
    REQUIRE(ReadAll(v54).substr(12) == "newer pipelines");
}

TEST_CASE("A new cache version skips an older file that isn't its cache",
          "[pipeline_cache_files][carry_over]") {
    TempDir temp;
    constexpr std::array<u32, 2> versions{53, 52};
    WriteCache(VideoCommon::VersionedPipelineCachePath(temp.path, "vulkan", 53), OtherMagic, 53,
               "another cache");
    WriteCache(VideoCommon::VersionedPipelineCachePath(temp.path, "vulkan", 52), Magic, 52,
               "older pipelines");
    REQUIRE(VideoCommon::CarryOverPipelineCache(temp.path, "vulkan", Magic, versions, 54).version ==
            52u);
    REQUIRE(ReadAll(VideoCommon::VersionedPipelineCachePath(temp.path, "vulkan", 54)).substr(12) ==
            "older pipelines");
}

TEST_CASE("A newer cache that can't be copied doesn't make way for an older one",
          "[pipeline_cache_files][carry_over]") {
    TempDir temp;
    constexpr std::array<u32, 2> versions{53, 52};
    const auto v54 = VideoCommon::VersionedPipelineCachePath(temp.path, "vulkan", 54);
    WriteCache(VideoCommon::VersionedPipelineCachePath(temp.path, "vulkan", 53), Magic, 53,
               "newer pipelines");
    WriteCache(VideoCommon::VersionedPipelineCachePath(temp.path, "vulkan", 52), Magic, 52,
               "older pipelines");
    // A directory where the copy goes makes it fail.
    auto temporary = v54;
    temporary += ".migrating";
    std::filesystem::create_directory(temporary);
    const auto carried =
        VideoCommon::CarryOverPipelineCache(temp.path, "vulkan", Magic, versions, 54);
    REQUIRE(carried.failed);
    REQUIRE_FALSE(carried.version);
    REQUIRE_FALSE(std::filesystem::exists(v54));
}

TEST_CASE("A new cache version without a compatible cache starts empty",
          "[pipeline_cache_files][carry_over]") {
    TempDir temp;
    constexpr std::array<u32, 2> versions{53, 52};
    WriteCache(VideoCommon::VersionedPipelineCachePath(temp.path, "vulkan", 51), Magic, 51,
               "another record layout");
    const auto carried =
        VideoCommon::CarryOverPipelineCache(temp.path, "vulkan", Magic, versions, 54);
    REQUIRE_FALSE(carried.version);
    REQUIRE_FALSE(carried.failed);
    REQUIRE_FALSE(
        std::filesystem::exists(VideoCommon::VersionedPipelineCachePath(temp.path, "vulkan", 54)));
}

TEST_CASE("The current Vulkan cache starts from the newest version it lists",
          "[pipeline_cache_files][carry_over]") {
    const std::span<const u32> versions{VideoCommon::CompatibleVulkanPipelineCacheVersions};
    if (versions.empty()) {
        return;
    }
    TempDir temp;
    WriteCache(VideoCommon::VersionedPipelineCachePath(temp.path, "vulkan", versions.front()),
               Magic, versions.front(), "pipelines");
    const auto carried =
        VideoCommon::CarryOverPipelineCache(temp.path, "vulkan", VideoCommon::PipelineCacheMagic,
                                            versions, VideoCommon::VulkanPipelineCacheVersion);
    REQUIRE(carried.version == versions.front());
    REQUIRE(VideoCommon::PipelineCacheHeaderMatches(
        VideoCommon::VersionedPipelineCachePath(temp.path, "vulkan",
                                                VideoCommon::VulkanPipelineCacheVersion),
        VideoCommon::PipelineCacheMagic, VideoCommon::VulkanPipelineCacheVersion));
}

TEST_CASE("A cleared cache stays empty while an older cache could replace it",
          "[pipeline_cache_files][carry_over]") {
    TempDir temp;
    constexpr std::array<u32, 2> versions{53, 52};
    const auto v53 = VideoCommon::VersionedPipelineCachePath(temp.path, "vulkan", 53);
    const auto v54 = VideoCommon::VersionedPipelineCachePath(temp.path, "vulkan", 54);
    WriteCache(v53, Magic, 53, "pipelines");
    const auto before_v53 = ReadAll(v53);
    REQUIRE(VideoCommon::CarryOverPipelineCache(temp.path, "vulkan", Magic, versions, 54).version);
    REQUIRE(VideoCommon::ClearPipelineCache(temp.path, "vulkan", Magic, versions, 54) ==
            VideoCommon::PipelineCacheClear::Cleared);
    REQUIRE(ReadAll(v54).size() == 12);
    REQUIRE(VideoCommon::PipelineCacheHeaderMatches(v54, Magic, 54));
    REQUIRE(ReadAll(v53) == before_v53);
    const auto carried =
        VideoCommon::CarryOverPipelineCache(temp.path, "vulkan", Magic, versions, 54);
    REQUIRE_FALSE(carried.version);
    REQUIRE_FALSE(carried.failed);
    REQUIRE(ReadAll(v54).size() == 12);
}

TEST_CASE("A cleared cache without an older one is removed", "[pipeline_cache_files][carry_over]") {
    TempDir temp;
    constexpr std::array<u32, 2> versions{53, 52};
    const auto v54 = VideoCommon::VersionedPipelineCachePath(temp.path, "vulkan", 54);
    REQUIRE(VideoCommon::ClearPipelineCache(temp.path, "vulkan", Magic, versions, 54) ==
            VideoCommon::PipelineCacheClear::Missing);
    WriteCache(v54, Magic, 54, "pipelines");
    REQUIRE(VideoCommon::ClearPipelineCache(temp.path, "vulkan", Magic, versions, 54) ==
            VideoCommon::PipelineCacheClear::Cleared);
    REQUIRE_FALSE(std::filesystem::exists(v54));
    REQUIRE(VideoCommon::ClearPipelineCache(temp.path, "vulkan", Magic, versions, 54) ==
            VideoCommon::PipelineCacheClear::Missing);
}

TEST_CASE("Truncated mesh state is rejected before a pipeline can be queued",
          "[pipeline_cache_files][mesh_serialization]") {
    TempDir temp;
    const auto path = temp.path / "truncated-mesh.bin";
    SerializedEnvironment source{Shader::Stage::Mesh};
    const std::array<const VideoCommon::GenericEnvironment*, 1> envs{&source};
    constexpr u64 key = 42;
    VideoCommon::SerializePipeline(key, envs, path, VideoCommon::VulkanPipelineCacheVersion,
                                   ReadProperties);
    std::filesystem::resize_file(path, std::filesystem::file_size(path) - sizeof(key) - 1);
    VideoCommon::LoadPipelines(
        {}, path, VideoCommon::VulkanPipelineCacheVersion,
        [](std::ifstream&, VideoCommon::FileEnvironment) { FAIL("Unexpected compute record"); },
        [](std::ifstream&, std::vector<VideoCommon::FileEnvironment>) {
            FAIL("Incomplete mesh state reached pipeline creation");
        });
    // Cut back to the header, which the next record follows.
    REQUIRE(std::filesystem::file_size(path) == Magic.size() + sizeof(u32));
    AppendPipeline(path, key, Shader::Stage::Mesh);
    REQUIRE(LoadKeys(path).keys == std::vector<u64>{key});
}

TEST_CASE("A damaged record drops only itself and the records after it",
          "[pipeline_cache_files][damaged_records]") {
    TempDir temp;
    const auto path = temp.path / "damaged.bin";
    AppendPipeline(path, 0, Shader::Stage::Fragment);
    AppendPipeline(path, 1, Shader::Stage::Task);
    const auto complete = std::filesystem::file_size(path);
    AppendPipeline(path, 2, Shader::Stage::Mesh);
    AppendPipeline(path, 3, Shader::Stage::Fragment);
    const auto before = ReadAll(path);
    const auto loaded = LoadKeys(path);
    REQUIRE(loaded.keys == std::vector<u64>{0, 1, 2, 3});
    REQUIRE(loaded.records.back().end == before.size());

    // A crash while the third record was appended: the fourth went after the damage.
    std::string damaged = before.substr(0, complete + 40) + before.substr(loaded.records[3].begin);
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(damaged.data(), static_cast<std::streamsize>(damaged.size()));
    }
    REQUIRE(LoadKeys(path).keys == std::vector<u64>{0, 1});
    REQUIRE(ReadAll(path) == before.substr(0, complete));
    REQUIRE(LoadKeys(path).keys == std::vector<u64>{0, 1});
    AppendPipeline(path, 4, Shader::Stage::Mesh);
    REQUIRE(LoadKeys(path).keys == std::vector<u64>{0, 1, 4});
}

TEST_CASE("A load without the lock leaves a damaged record for one that holds it",
          "[pipeline_cache_files][damaged_records][file_lock]") {
    TempDir temp;
    const auto path = temp.path / "appending.bin";
    AppendPipeline(path, 0, Shader::Stage::Fragment);
    const auto complete = std::filesystem::file_size(path);
    AppendPipeline(path, 1, Shader::Stage::Fragment);
    // Another emulator partway through appending the second record
    const std::string partial = ReadAll(path).substr(0, complete + 40);
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(partial.data(), static_cast<std::streamsize>(partial.size()));
    }
    {
        const Common::FS::FileLock other{VideoCommon::PipelineCacheLockPath(path)};
        REQUIRE(other.Held());
        REQUIRE(LoadKeys(path).keys == std::vector<u64>{0});
        REQUIRE(ReadAll(path) == partial);
    }
    REQUIRE(LoadKeys(path).keys == std::vector<u64>{0});
    REQUIRE(std::filesystem::file_size(path) == complete);
}

TEST_CASE("An append is left out while another emulator keeps the cache's lock",
          "[pipeline_cache_files][file_lock]") {
    TempDir temp;
    const auto path = temp.path / "busy.bin";
    AppendPipeline(path, 0, Shader::Stage::Fragment);
    const auto before = ReadAll(path);
    {
        const Common::FS::FileLock other{VideoCommon::PipelineCacheLockPath(path)};
        REQUIRE(other.Held());
        AppendPipeline(path, 1, Shader::Stage::Fragment);
        REQUIRE(ReadAll(path) == before);
    }
    AppendPipeline(path, 2, Shader::Stage::Fragment);
    REQUIRE(LoadKeys(path).keys == std::vector<u64>{0, 2});
}

TEST_CASE("A record whose key is cut short never reaches pipeline creation",
          "[pipeline_cache_files][damaged_records]") {
    TempDir temp;
    const auto path = temp.path / "short-key.bin";
    AppendPipeline(path, 7, Shader::Stage::Fragment);
    const auto complete = std::filesystem::file_size(path);
    AppendPipeline(path, 8, Shader::Stage::Fragment);
    std::filesystem::resize_file(path, std::filesystem::file_size(path) - 1);
    REQUIRE(LoadKeys(path).keys == std::vector<u64>{7});
    REQUIRE(std::filesystem::file_size(path) == complete);
}

TEST_CASE("Damaged counts are dropped before anything is allocated for them",
          "[pipeline_cache_files][damaged_records]") {
    // An environment's sizes and fields as Deserialize reads them (its code is at 0, so 8 bytes),
    // then bytes for what follows.
    const auto append_damaged = [](const std::filesystem::path& file, u32 num_envs, u64 code_size,
                                   u64 num_cbuf_values, size_t padding) {
        std::ofstream stream(file, std::ios::binary | std::ios::app);
        const std::array<u64, 6> sizes{code_size, 0, 0, num_cbuf_values, 0, 0};
        std::array<u32, 7> fields{};
        fields.back() = static_cast<u32>(Shader::Stage::Fragment);
        const std::string bytes(padding, '\0');
        stream.write(reinterpret_cast<const char*>(&num_envs), sizeof(num_envs));
        stream.write(reinterpret_cast<const char*>(sizes.data()), sizeof(sizes));
        stream.write(reinterpret_cast<const char*>(fields.data()), sizeof(fields));
        stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    };
    // Environment count, code size, constant buffer value count and the bytes after them, of each
    // damaged record
    constexpr std::array<std::tuple<u32, u64, u64, size_t>, 8> damaged{{
        {0, 0, 0, 256},
        {0xffffffff, 0, 0, 256},
        {1, u64{1} << 62, 0, 256},
        {1, ~u64{}, 0, 256},
        {1, 0, u64{1} << 60, 256},
        {6, 300, 0, 256},
        // Code that fits in the file but isn't the size of its address range
        {1, 16, 0, 256},
        // More values than a record holds, which fit in the file
        {1, 8, (u64{1} << 16) + 1, 1 << 20},
    }};
    TempDir temp;
    const auto path = temp.path / "counts.bin";
    AppendPipeline(path, 5, Shader::Stage::Fragment);
    const auto before = ReadAll(path);
    for (const auto& [num_envs, code_size, num_cbuf_values, padding] : damaged) {
        append_damaged(path, num_envs, code_size, num_cbuf_values, padding);
        AppendPipeline(path, 6, Shader::Stage::Fragment);
        REQUIRE(LoadKeys(path).keys == std::vector<u64>{5});
        REQUIRE(ReadAll(path) == before);
    }
}

TEST_CASE("A compute record whose key disagrees with its environment is damaged",
          "[pipeline_cache_files][damaged_records]") {
    TempDir temp;
    const auto path = temp.path / "compute-key.bin";
    AppendComputePipeline(path, 1, false);
    const auto complete = ReadAll(path);
    // The key read from another launch than the environment, as damage could make it
    SerializedEnvironment source{Shader::Stage::Compute};
    const std::array<const VideoCommon::GenericEnvironment*, 1> envs{&source};
    for (const auto& key :
         {Vulkan::ComputePipelineCacheKey{
              .unique_hash = 77, .shared_memory_size = 0x100, .workgroup_size{}},
          Vulkan::ComputePipelineCacheKey{
              .unique_hash = 77, .shared_memory_size = 0, .workgroup_size{32, 1, 1}}}) {
        VideoCommon::SerializePipeline(key, envs, path, VideoCommon::VulkanPipelineCacheVersion,
                                       ReadProperties);
        AppendComputePipeline(path, 2, false);
        REQUIRE(LoadComputeTranslations(path).size() == 1);
        REQUIRE(ReadAll(path) == complete);
    }
}

TEST_CASE("Running out of memory while loading stops the load and keeps the file",
          "[pipeline_cache_files][damaged_records]") {
    TempDir temp;
    const auto path = temp.path / "memory.bin";
    for (u64 key = 0; key < 3; ++key) {
        AppendPipeline(path, key, Shader::Stage::Fragment);
    }
    const auto before = ReadAll(path);
    size_t loaded{};
    VideoCommon::LoadPipelines(
        {}, path, VideoCommon::VulkanPipelineCacheVersion,
        [](std::ifstream&, VideoCommon::FileEnvironment) { FAIL("Unexpected compute record"); },
        [&](std::ifstream& file, std::vector<VideoCommon::FileEnvironment>) {
            u64 key{};
            file.read(reinterpret_cast<char*>(&key), sizeof(key));
            if (++loaded == 2) {
                throw std::bad_alloc{};
            }
        });
    REQUIRE(loaded == 2);
    REQUIRE(ReadAll(path) == before);
    REQUIRE(LoadKeys(path).keys == std::vector<u64>{0, 1, 2});
}

TEST_CASE("A cached environment tells when the translator asked for a value it lacks",
          "[pipeline_cache_files][incomplete_records]") {
    TempDir temp;
    const auto path = temp.path / "incomplete.bin";
    AppendPipeline(path, 9, Shader::Stage::Fragment);
    const auto lookups = std::to_array<std::function<void(VideoCommon::FileEnvironment&)>>({
        [](auto& env) { static_cast<void>(env.ReadCbufValue(1, 20)); },
        [](auto& env) { static_cast<void>(env.ReadTextureType(4)); },
        [](auto& env) { static_cast<void>(env.ReadTexturePixelFormat(3)); },
        [](auto& env) { static_cast<void>(env.IsCbufStorageBufferPointer(1, 16)); },
        [](auto& env) { static_cast<void>(env.IsCbufIndirectStorageBufferPointer(1, 16, 8)); },
    });
    for (const auto& lookup : lookups) {
        bool loaded = false;
        VideoCommon::LoadPipelines(
            {}, path, VideoCommon::VulkanPipelineCacheVersion,
            [](std::ifstream&, VideoCommon::FileEnvironment) { FAIL("Unexpected compute record"); },
            [&](std::ifstream& file, std::vector<VideoCommon::FileEnvironment> envs) {
                u64 key{};
                file.read(reinterpret_cast<char*>(&key), sizeof(key));
                auto& env = envs.front();
                REQUIRE(env.ReadCbufValue(1, 16) == 0x1357);
                REQUIRE(env.ReadTextureType(3) == Shader::TextureType::Color2D);
                // Code outside the record is not a missing value: a live environment fails too.
                REQUIRE_THROWS_AS(static_cast<void>(env.ReadInstruction(0x1000)),
                                  Shader::LogicError);
                REQUIRE_FALSE(env.LacksRecordedValue(ReadProperties));
                REQUIRE_THROWS_AS(lookup(env), Shader::LogicError);
                REQUIRE(env.LacksRecordedValue(ReadProperties));
                loaded = true;
            });
        REQUIRE(loaded);
    }
}

TEST_CASE("A missing value is conclusive where the record was made, or for an older build",
          "[pipeline_cache_files][incomplete_records]") {
    TempDir temp;
    const auto path = temp.path / "hosts.bin";
    AppendPipeline(path, 1, Shader::Stage::Fragment);
    AppendOlderPipeline(path, 2);
    std::vector<bool> lacks_here;
    std::vector<bool> lacks_elsewhere;
    VideoCommon::LoadPipelines(
        {}, path, VideoCommon::VulkanPipelineCacheVersion,
        [](std::ifstream&, VideoCommon::FileEnvironment) { FAIL("Unexpected compute record"); },
        [&](std::ifstream& file, std::vector<VideoCommon::FileEnvironment> envs) {
            u64 key{};
            file.read(reinterpret_cast<char*>(&key), sizeof(key));
            auto& env = envs.front();
            REQUIRE(env.ReadCbufValue(1, 16) == 0x1357);
            // The read properties are not a value translation can read.
            REQUIRE_THROWS_AS(static_cast<void>(env.ReadCbufValue(~0U, 0)), Shader::LogicError);
            lacks_here.push_back(env.LacksRecordedValue(ReadProperties));
            lacks_elsewhere.push_back(env.LacksRecordedValue(ReadProperties ^ 1));
        });
    // Another host's translation can read other values: its record may hold all of them.
    REQUIRE(lacks_here == std::vector<bool>{true, true});
    REQUIRE(lacks_elsewhere == std::vector<bool>{false, true});
}

TEST_CASE("Removing pipelines keeps every other byte of the cache",
          "[pipeline_cache_files][remove_pipelines]") {
    TempDir temp;
    const auto path = temp.path / "remove.bin";
    auto temporary = path;
    temporary += ".tmp";
    for (u64 key = 0; key < 5; ++key) {
        AppendPipeline(path, key, key % 2 == 0 ? Shader::Stage::Fragment : Shader::Stage::Mesh);
    }
    const auto loaded = LoadKeys(path);
    REQUIRE(loaded.keys == std::vector<u64>{0, 1, 2, 3, 4});
    REQUIRE(loaded.records.front().begin == Magic.size() + sizeof(u32));
    const auto before = ReadAll(path);
    const auto& records = loaded.records;
    const auto bytes = [&](size_t first, size_t last) {
        return before.substr(records[first].begin, records[last].end - records[first].begin);
    };
    constexpr u32 version = VideoCommon::VulkanPipelineCacheVersion;

    // Records that are not complete records inside this file, as the load read them, leave it as it
    // was.
    const auto refused = [&](std::vector<VideoCommon::PipelineRecord> removed, u32 cache_version,
                             u64 loaded_size) {
        REQUIRE_FALSE(
            VideoCommon::RemovePipelines({}, path, cache_version, loaded_size, std::move(removed)));
        REQUIRE(ReadAll(path) == before);
        REQUIRE_FALSE(std::filesystem::exists(temporary));
    };
    const u64 size{before.size()};
    refused({{records[1].begin, before.size() + 1, records[1].hash}}, version, size);
    refused({{4, records[0].end, records[0].hash}}, version, size);
    refused({records[1], {records[1].begin + 1, records[2].end, records[2].hash}}, version, size);
    refused({{records[2].end, records[2].begin, records[2].hash}}, version, size);
    refused({{records[1].begin, records[1].end, records[1].hash ^ 1}}, version, size);
    refused({records[1]}, version + 1, size);
    refused({records[1]}, version, size + 1);
    // A copy goes only while its original holds the same bytes.
    refused({{records[3].begin, records[3].end, records[3].hash, records[1].begin}}, version, size);
    refused({}, version, size);
    std::stop_source stop;
    stop.request_stop();
    REQUIRE_FALSE(
        VideoCommon::RemovePipelines(stop.get_token(), path, version, size, {records[1]}));
    REQUIRE(ReadAll(path) == before);
    REQUIRE_FALSE(std::filesystem::exists(temporary));

    REQUIRE(VideoCommon::RemovePipelines({}, path, version, size,
                                         {records[3], records[1], records[3]}));
    REQUIRE(ReadAll(path) == before.substr(0, records[0].end) + bytes(2, 2) + bytes(4, 4));
    REQUIRE_FALSE(std::filesystem::exists(temporary));
    REQUIRE(LoadKeys(path).keys == std::vector<u64>{0, 2, 4});
}

TEST_CASE("A record appended after the load stays when the file is rewritten",
          "[pipeline_cache_files][remove_pipelines]") {
    TempDir temp;
    const auto path = temp.path / "appended.bin";
    for (u64 key = 0; key < 3; ++key) {
        AppendPipeline(path, key, Shader::Stage::Fragment);
    }
    const auto loaded = LoadKeys(path);
    const auto before = ReadAll(path);
    AppendPipeline(path, 3, Shader::Stage::Mesh);
    const auto appended = ReadAll(path).substr(before.size());
    REQUIRE(VideoCommon::RemovePipelines({}, path, VideoCommon::VulkanPipelineCacheVersion,
                                         before.size(), {loaded.records[1]}));
    REQUIRE(ReadAll(path) == before.substr(0, loaded.records[1].begin) +
                                 before.substr(loaded.records[1].end) + appended);
    REQUIRE(LoadKeys(path).keys == std::vector<u64>{0, 2, 3});
}

TEST_CASE("A rewrite refuses records that another process has moved since the load",
          "[pipeline_cache_files][remove_pipelines]") {
    TempDir temp;
    const auto path = temp.path / "two-instances.bin";
    for (u64 key = 0; key < 10; ++key) {
        AppendPipeline(path, key, key % 3 == 0 ? Shader::Stage::Mesh : Shader::Stage::Fragment);
    }
    // Two emulators load the same file and find the same records to remove. One rewrites it first
    // and appends to it, so that it is as long as before.
    const auto first = LoadKeys(path);
    const auto second = LoadKeys(path);
    const u64 size{std::filesystem::file_size(path)};
    constexpr u32 version = VideoCommon::VulkanPipelineCacheVersion;
    REQUIRE(VideoCommon::RemovePipelines({}, path, version, size,
                                         {first.records[2], first.records[5]}));
    for (u64 key = 10; std::filesystem::file_size(path) < size; ++key) {
        AppendPipeline(path, key, Shader::Stage::Fragment);
    }
    const auto changed = ReadAll(path);
    // The other's offsets now name other bytes: its rewrite leaves the file as it is.
    REQUIRE_FALSE(VideoCommon::RemovePipelines({}, path, version, size,
                                               {second.records[2], second.records[5]}));
    REQUIRE_FALSE(VideoCommon::RemovePipelines({}, path, version, size, {second.records[6]}));
    REQUIRE(ReadAll(path) == changed);
    REQUIRE(LoadKeys(path).keys == std::vector<u64>{0, 1, 3, 4, 6, 7, 8, 9, 10, 11});
}

TEST_CASE("A rewrite waits for a later boot while another process holds the cache's lock",
          "[pipeline_cache_files][remove_pipelines]") {
    TempDir temp;
    const auto path = temp.path / "locked.bin";
    for (u64 key = 0; key < 3; ++key) {
        AppendPipeline(path, key, Shader::Stage::Fragment);
    }
    const auto loaded = LoadKeys(path);
    const auto before = ReadAll(path);
    constexpr u32 version = VideoCommon::VulkanPipelineCacheVersion;
    {
        const Common::FS::FileLock lock{VideoCommon::PipelineCacheLockPath(path)};
        REQUIRE(lock.Held());
        REQUIRE_FALSE(
            VideoCommon::RemovePipelines({}, path, version, before.size(), {loaded.records[1]}));
        REQUIRE(ReadAll(path) == before);
    }
    REQUIRE(VideoCommon::RemovePipelines({}, path, version, before.size(), {loaded.records[1]}));
    REQUIRE(LoadKeys(path).keys == std::vector<u64>{0, 2});
}

TEST_CASE("A lock wait gives up after its time or when stopped, and only tries a stuck lock",
          "[pipeline_cache_files][file_lock]") {
    using namespace std::chrono_literals;
    TempDir temp;
    const auto path = temp.path / "cache.lock";
    auto elapsed = [start = std::chrono::steady_clock::now()]() mutable {
        const auto now = std::chrono::steady_clock::now();
        return now - std::exchange(start, now);
    };
    // Another open file of the lock is kept out as another process is.
    std::optional<Common::FS::FileLock> other{std::in_place, path};
    REQUIRE(other->Held());
    elapsed();
    const Common::FS::FileLock waited{path, 50ms};
    REQUIRE_FALSE(waited.Held());
    REQUIRE(waited.Busy());
    REQUIRE(elapsed() >= 50ms);
    // Held through a whole wait: until it is free again, a wait only tries once.
    REQUIRE_FALSE(Common::FS::FileLock{path, 10s}.Held());
    REQUIRE(elapsed() < 5s);
    other.reset();
    REQUIRE(Common::FS::FileLock{path, 10s}.Held());
    other.emplace(path);
    std::stop_source stop;
    std::thread stopper{[&stop] {
        std::this_thread::sleep_for(50ms);
        stop.request_stop();
    }};
    elapsed();
    REQUIRE_FALSE(Common::FS::FileLock{path, 10s, stop.get_token()}.Held());
    REQUIRE(elapsed() < 5s);
    stopper.join();
}

TEST_CASE("A cancelled load stops waiting for another emulator's lock",
          "[pipeline_cache_files][file_lock]") {
    TempDir temp;
    const auto path = temp.path / "waiting.bin";
    AppendPipeline(path, 1, Shader::Stage::Fragment);
    const Common::FS::FileLock other{VideoCommon::PipelineCacheLockPath(path)};
    REQUIRE(other.Held());
    std::stop_source stop;
    std::thread stopper{[&stop] {
        std::this_thread::sleep_for(std::chrono::milliseconds{50});
        stop.request_stop();
    }};
    const auto start = std::chrono::steady_clock::now();
    VideoCommon::LoadPipelines(
        stop.get_token(), path, VideoCommon::VulkanPipelineCacheVersion,
        [](std::ifstream&, VideoCommon::FileEnvironment) { FAIL("Unexpected compute record"); },
        [](std::ifstream&, std::vector<VideoCommon::FileEnvironment>) {
            FAIL("Cancelled load reached pipeline creation");
        });
    REQUIRE(std::chrono::steady_clock::now() - start < VideoCommon::PipelineCacheLockWait);
    stopper.join();
}

TEST_CASE("Only the guest environment caches have a lock", "[pipeline_cache_files][carry_over]") {
    TempDir temp;
    constexpr std::array<u32, 2> versions{53, 52};
    for (const auto& [stem, magic] :
         {std::pair{"vulkan", Magic}, std::pair{"driver", OtherMagic}}) {
        WriteCache(VideoCommon::VersionedPipelineCachePath(temp.path, stem, 53), magic, 53, "53");
        REQUIRE(VideoCommon::CarryOverPipelineCache(temp.path, stem, magic, versions, 54).version);
        REQUIRE(VideoCommon::ClearPipelineCache(temp.path, stem, magic, versions, 54) ==
                VideoCommon::PipelineCacheClear::Cleared);
        const bool locked{std::filesystem::exists(VideoCommon::PipelineCacheLockPath(
            VideoCommon::VersionedPipelineCachePath(temp.path, stem, 54)))};
        REQUIRE(locked == (magic == Magic));
    }
}

TEST_CASE("A failed rewrite is tried again only once the file has changed",
          "[pipeline_cache_files][remove_pipelines]") {
    TempDir temp;
    const auto path = temp.path / "failing.bin";
    auto temporary = path;
    temporary += ".tmp";
    for (u64 key = 0; key < 3; ++key) {
        AppendPipeline(path, key, Shader::Stage::Fragment);
    }
    const auto loaded = LoadKeys(path);
    const auto before = ReadAll(path);
    constexpr u32 version = VideoCommon::VulkanPipelineCacheVersion;
    // The new file can't be written where a directory is.
    std::filesystem::create_directory(temporary);
    REQUIRE_FALSE(
        VideoCommon::RemovePipelines({}, path, version, before.size(), {loaded.records[1]}));
    REQUIRE(ReadAll(path) == before);
    REQUIRE_FALSE(std::filesystem::exists(temporary));
    // It could be written now, but the file is as it was when that failed.
    REQUIRE_FALSE(
        VideoCommon::RemovePipelines({}, path, version, before.size(), {loaded.records[1]}));
    REQUIRE(ReadAll(path) == before);
    AppendPipeline(path, 3, Shader::Stage::Fragment);
    REQUIRE(VideoCommon::RemovePipelines({}, path, version, before.size(), {loaded.records[1]}));
    REQUIRE(LoadKeys(path).keys == std::vector<u64>{0, 2, 3});
    auto failed = path;
    failed += ".rewrite-failed";
    REQUIRE_FALSE(std::filesystem::exists(failed));
}

TEST_CASE("A copy of a record is removed while its original stays",
          "[pipeline_cache_files][remove_pipelines]") {
    TempDir temp;
    const auto path = temp.path / "copies.bin";
    AppendPipeline(path, 1, Shader::Stage::Fragment);
    AppendPipeline(path, 7, Shader::Stage::Fragment);
    AppendPipeline(path, 1, Shader::Stage::Fragment);
    const auto loaded = LoadKeys(path);
    const auto& records = loaded.records;
    REQUIRE(records[0].hash == records[2].hash);
    auto copy = records[2];
    copy.copy_of = records[0].begin;
    const auto size = std::filesystem::file_size(path);
    REQUIRE(VideoCommon::RemovePipelines({}, path, VideoCommon::VulkanPipelineCacheVersion, size,
                                         {copy}));
    REQUIRE(LoadKeys(path).keys == std::vector<u64>{1, 7});
}

TEST_CASE("Compute translations lacking a value go, but not one the order is relative to",
          "[pipeline_cache_files][incomplete_records]") {
    TempDir temp;
    const auto path = temp.path / "compute.bin";
    // Which of three translations of a key go once the third translated, the two before it lacking
    // a value: both bind a pointer, the third binds it as well or not.
    const auto removed = [&](bool third_binds_pointer, u32 second_read_properties) {
        std::filesystem::remove(path);
        AppendComputePipeline(path, 1, true);
        AppendComputePipeline(path, 2, true, second_read_properties);
        AppendComputePipeline(path, 3, third_binds_pointer);
        auto candidates = LoadComputeTranslations(path);
        REQUIRE(candidates.size() == 3);
        AskMissingValue(candidates[0].env);
        AskMissingValue(candidates[1].env);
        std::vector<VideoCommon::PipelineRecord> incomplete;
        std::vector<VideoCommon::PipelineRecord> copies;
        Vulkan::AddUnchosenComputeTranslations(candidates, 2, ReadProperties, incomplete, copies);
        REQUIRE(copies.empty());
        std::vector<size_t> indices;
        for (size_t index = 0; index < candidates.size(); ++index) {
            if (std::ranges::find(incomplete, candidates[index].record) != incomplete.end()) {
                indices.push_back(index);
            }
        }
        return indices;
    };
    REQUIRE(removed(true, ReadProperties) == std::vector<size_t>{0, 1});
    // The order of the others is relative to the first, so it stays while the next one kept binds
    // other pointers.
    REQUIRE(removed(false, ReadProperties) == std::vector<size_t>{1});
    // Made on another host, the second may hold all that translation reads there.
    REQUIRE(removed(true, ReadProperties ^ 1) == std::vector<size_t>{0});
}

TEST_CASE("A copy of a compute translation goes while its original stays",
          "[pipeline_cache_files][remove_pipelines]") {
    TempDir temp;
    const auto path = temp.path / "compute-copies.bin";
    AppendComputePipeline(path, 1, false);
    AppendComputePipeline(path, 2, false);
    AppendComputePipeline(path, 1, false);
    const auto candidates = LoadComputeTranslations(path);
    REQUIRE(candidates.size() == 3);
    REQUIRE(candidates[0].record.hash == candidates[2].record.hash);
    const auto copies_when = [&](size_t chosen) {
        std::vector<VideoCommon::PipelineRecord> incomplete;
        std::vector<VideoCommon::PipelineRecord> copies;
        Vulkan::AddUnchosenComputeTranslations(candidates, chosen, ReadProperties, incomplete,
                                               copies);
        REQUIRE(incomplete.empty());
        return copies;
    };
    auto copy = candidates[2].record;
    copy.copy_of = candidates[0].record.begin;
    REQUIRE(copies_when(0) == std::vector{copy});
    REQUIRE(copies_when(candidates.size()) == std::vector{copy});
    // The one that translated stays.
    REQUIRE(copies_when(2).empty());
}

TEST_CASE("A later graphics record of a key goes only when its key is the first one's",
          "[pipeline_cache_files][remove_pipelines]") {
    TempDir temp;
    const auto path = temp.path / "graphics-keys.bin";
    Vulkan::GraphicsPipelineCacheKey key{};
    key.unique_hashes[4] = 1;
    auto other = key;
    other.unique_hashes[4] = 2;
    SerializedEnvironment source{Shader::Stage::Fragment};
    const std::array<const VideoCommon::GenericEnvironment*, 1> envs{&source};
    for (const auto& written : {key, key, other}) {
        VideoCommon::SerializePipeline(written, envs, path, VideoCommon::VulkanPipelineCacheVersion,
                                       ReadProperties);
    }
    std::vector<VideoCommon::PipelineRecord> records;
    u64 begin{Magic.size() + sizeof(u32)};
    VideoCommon::LoadPipelines(
        {}, path, VideoCommon::VulkanPipelineCacheVersion,
        [](std::ifstream&, VideoCommon::FileEnvironment) { FAIL("Unexpected compute record"); },
        [&](std::ifstream& stream, std::vector<VideoCommon::FileEnvironment>) {
            Vulkan::GraphicsPipelineCacheKey read{};
            stream.read(reinterpret_cast<char*>(&read), sizeof(read));
            records.push_back(VideoCommon::ReadPipelineRecord(stream, begin));
            begin = records.back().end;
        });
    REQUIRE(records.size() == 3);
    // Both have the first one's key hash, as far as this is concerned.
    const u64 first_key{records[0].end - sizeof(Vulkan::GraphicsPipelineCacheKey)};
    const std::array<Vulkan::DuplicateGraphicsRecord, 2> duplicates{{
        {records[1], first_key},
        {records[2], first_key},
    }};
    std::vector<VideoCommon::PipelineRecord> equal;
    Vulkan::AddEqualGraphicsKeys(path, duplicates, equal);
    REQUIRE(equal == std::vector{records[1]});
}

TEST_CASE("An incomplete graphics record's key is recorded again unless a later record has it",
          "[pipeline_cache_files][incomplete_records]") {
    constexpr u64 key_size{sizeof(Vulkan::GraphicsPipelineCacheKey)};
    const VideoCommon::PipelineRecord alone{12, 2000, 1};
    const VideoCommon::PipelineRecord first{2000, 4000, 2};
    const VideoCommon::PipelineRecord later{4000, 6000, 3};
    std::vector<Vulkan::DuplicateGraphicsRecord> duplicates{{later, first.end - key_size}};
    std::unordered_set<u64> disk_pipelines{7, 8, 9};
    const std::array<std::pair<u64, VideoCommon::PipelineRecord>, 2> incomplete{{
        {7, alone},
        {8, first},
    }};
    std::vector<VideoCommon::PipelineRecord> removed;
    Vulkan::AddIncompleteGraphics(incomplete, duplicates, disk_pipelines, removed);
    REQUIRE(removed == std::vector{alone, first});
    // The later record takes the place of the one removed: the next boot builds it.
    REQUIRE(duplicates.empty());
    REQUIRE(disk_pipelines == std::unordered_set<u64>{8, 9});
}

TEST_CASE("A rewrite cut off is removed at the next load",
          "[pipeline_cache_files][remove_pipelines]") {
    TempDir temp;
    const auto path = temp.path / "cut-off.bin";
    auto temporary = path;
    temporary += ".tmp";
    AppendPipeline(path, 1, Shader::Stage::Fragment);
    WriteCache(temporary, Magic, VideoCommon::VulkanPipelineCacheVersion, "half a rewrite");
    REQUIRE(LoadKeys(path).keys == std::vector<u64>{1});
    REQUIRE_FALSE(std::filesystem::exists(temporary));
}

TEST_CASE("Records whose stages or key disagree with their environments are damaged",
          "[pipeline_cache_files][damaged_records]") {
    TempDir temp;
    const auto path = temp.path / "stages.bin";
    constexpr std::array fragment{Shader::Stage::Fragment};
    AppendKeyedPipeline(path, {1, 0, 0, 0, 0, 0}, fragment);
    const auto complete = ReadAll(path);
    const auto damaged = [&](const std::array<u64, 6>& key, std::span<const Shader::Stage> stages) {
        AppendKeyedPipeline(path, key, stages);
        AppendKeyedPipeline(path, {3, 0, 0, 0, 0, 0}, fragment);
        REQUIRE(LoadCheckedKeys(path) == std::vector<u64>{1});
        REQUIRE(ReadAll(path) == complete);
    };
    // A key naming two shaders for one environment, and one shader for two
    damaged({2, 0, 0, 0, 0, 0x22}, fragment);
    damaged({2, 0, 0, 0, 0, 0}, std::array{Shader::Stage::VertexB, Shader::Stage::Fragment});
    // A stage no environment has, and a compute environment in a graphics record
    damaged({2, 0, 0, 0, 0, 0}, std::array{static_cast<Shader::Stage>(42)});
    damaged({2, 0, 0, 0, 0, 0x22}, std::array{Shader::Stage::Fragment, Shader::Stage::Compute});
    // A compute record holds one environment.
    damaged({2, 0, 0, 0, 0, 0x22}, std::array{Shader::Stage::Compute, Shader::Stage::Compute});
}

TEST_CASE("Cancelled mesh loading preserves its disk cache and queues no work",
          "[pipeline_cache_files][mesh_serialization]") {
    TempDir temp;
    const auto path = temp.path / "cancelled-mesh.bin";
    SerializedEnvironment source{Shader::Stage::Mesh};
    const std::array<const VideoCommon::GenericEnvironment*, 1> envs{&source};
    constexpr u64 key = 42;
    VideoCommon::SerializePipeline(key, envs, path, VideoCommon::VulkanPipelineCacheVersion,
                                   ReadProperties);
    const auto before = ReadAll(path);
    std::stop_source stop;
    stop.request_stop();
    VideoCommon::LoadPipelines(
        stop.get_token(), path, VideoCommon::VulkanPipelineCacheVersion,
        [](std::ifstream&, VideoCommon::FileEnvironment) { FAIL("Unexpected compute record"); },
        [](std::ifstream&, std::vector<VideoCommon::FileEnvironment>) {
            FAIL("Cancelled load reached pipeline creation");
        });
    REQUIRE(ReadAll(path) == before);
}

TEST_CASE("Pipeline cache names are version-suffixed per build", "[pipeline_cache_files]") {
    const std::filesystem::path dir{"shader"};
    CHECK(VideoCommon::VersionedPipelineCachePath(dir, "vulkan", 39).filename() ==
          "vulkan.fork-v39.bin");
    CHECK(VideoCommon::VersionedPipelineCachePath(dir, "vulkan_pipelines", 39).filename() ==
          "vulkan_pipelines.fork-v39.bin");
    CHECK(VideoCommon::VersionedPipelineCachePath(dir, "vulkan", 18) !=
          VideoCommon::VersionedPipelineCachePath(dir, "vulkan", 39));
}

TEST_CASE("The binary names the pipeline cache file it opens", "[pipeline_cache_files]") {
    const std::string marker{VideoCommon::VulkanPipelineCacheMarker.data()};
    CHECK(marker == "lichii-pipeline-cache=" +
                        VideoCommon::VersionedPipelineCachePath(
                            "shader", "vulkan", VideoCommon::VulkanPipelineCacheVersion)
                            .filename()
                            .string());
    CHECK(marker.size() + 1 <= VideoCommon::VulkanPipelineCacheMarker.size());
}

TEST_CASE("A legacy cache of this version is adopted with its contents",
          "[pipeline_cache_files]") {
    TempDir temp;
    const auto legacy = temp.path / "vulkan.bin";
    const auto versioned = VideoCommon::VersionedPipelineCachePath(temp.path, "vulkan", 39);
    WriteCache(legacy, Magic, 39, "pipelines");
    const std::string before = ReadAll(legacy);
    REQUIRE(VideoCommon::AdoptLegacyPipelineCache(legacy, versioned, Magic, 39));
    CHECK_FALSE(std::filesystem::exists(legacy));
    CHECK(ReadAll(versioned) == before);
    // Idempotent: nothing left to adopt.
    CHECK_FALSE(VideoCommon::AdoptLegacyPipelineCache(legacy, versioned, Magic, 39));
}

TEST_CASE("Another build's legacy cache is never touched", "[pipeline_cache_files]") {
    TempDir temp;
    const auto legacy = temp.path / "vulkan.bin";
    const auto versioned = VideoCommon::VersionedPipelineCachePath(temp.path, "vulkan", 39);
    WriteCache(legacy, Magic, 18, "official");
    const std::string before = ReadAll(legacy);
    CHECK_FALSE(VideoCommon::AdoptLegacyPipelineCache(legacy, versioned, Magic, 39));
    CHECK(ReadAll(legacy) == before);
    CHECK_FALSE(std::filesystem::exists(versioned));

    WriteCache(legacy, OtherMagic, 39, "wrong magic");
    CHECK_FALSE(VideoCommon::AdoptLegacyPipelineCache(legacy, versioned, Magic, 39));
    CHECK(std::filesystem::exists(legacy));

    // Truncated header.
    {
        std::ofstream stream(legacy, std::ios::binary | std::ios::trunc);
        stream.write(Magic.data(), 4);
    }
    CHECK_FALSE(VideoCommon::AdoptLegacyPipelineCache(legacy, versioned, Magic, 39));
    CHECK(std::filesystem::exists(legacy));
}

TEST_CASE("An existing versioned cache is never overwritten by a legacy one",
          "[pipeline_cache_files]") {
    TempDir temp;
    const auto legacy = temp.path / "vulkan.bin";
    const auto versioned = VideoCommon::VersionedPipelineCachePath(temp.path, "vulkan", 39);
    WriteCache(legacy, Magic, 39, "older");
    WriteCache(versioned, Magic, 39, "current");
    const std::string current = ReadAll(versioned);
    CHECK_FALSE(VideoCommon::AdoptLegacyPipelineCache(legacy, versioned, Magic, 39));
    CHECK(ReadAll(versioned) == current);
    CHECK(std::filesystem::exists(legacy));
    CHECK_FALSE(VideoCommon::AdoptLegacyPipelineCache(temp.path / "missing.bin", versioned,
                                                      Magic, 39));
}

TEST_CASE("Cached compute failures retain their cause and the current dispatch address",
          "[pipeline_cache][diagnostics]") {
    const Vulkan::ComputePipelineCacheKey key{
        .unique_hash = 0x1234,
        .shared_memory_size = 1024,
        .workgroup_size = {32, 1, 1},
    };
    std::unordered_map<Vulkan::ComputePipelineCacheKey, Vulkan::ComputePipelineCacheEntry> cache;
    const std::string cause =
        "SPIR-V emission failed at IR opcode UndefU8: SPIR-V Instruction is not implemented";
    cache[key].failure = cause;
    for (const GPUVAddr address : {0x211ddd9000ULL, 0x21ebdfa000ULL}) {
        const auto [entry, inserted] = cache.try_emplace(key);
        REQUIRE_FALSE(inserted);
        CHECK(entry->second.Get(false, address, key) == nullptr);
        try {
            static_cast<void>(entry->second.Get(true, address, key));
            FAIL("Native compute failure was not propagated");
        } catch (const Shader::RuntimeError& exception) {
            const std::string message{exception.what()};
            CHECK(message.find(cause) != std::string::npos);
            CHECK(message.find(fmt::format("address={:#x}", address)) != std::string::npos);
            CHECK(message.find(fmt::format("shader={:#016x}", key.unique_hash)) != std::string::npos);
            CHECK(message.find(fmt::format("pipeline={:#016x}", key.Hash())) != std::string::npos);
        }
        CHECK(entry->second.failure == cause);
        CHECK_FALSE(entry->second.pipeline);
    }
    auto other = key;
    other.workgroup_size[0] = 64;
    const auto [entry, inserted] = cache.try_emplace(other);
    REQUIRE(inserted);
    CHECK(entry->second.failure.empty());
    CHECK(entry->second.Get(false, 0x2120633c00, other) == nullptr);
    REQUIRE_THROWS_AS(entry->second.Get(true, 0x2120633c00, other), Shader::RuntimeError);
}

namespace {

// A compute environment with storage-pointer classifications, as a native compute program leaves
// them in the pipeline cache: (index, offset) slots with their two constant-buffer words and the
// IsCbufStorageBufferPointer answer.
struct PointerSlot {
    u32 index;
    u32 offset;
    u32 low;
    u32 high;
    bool mapped;
};

class PointerEnvironment final : public VideoCommon::GenericEnvironment {
public:
    PointerEnvironment(u32 marker, std::initializer_list<PointerSlot> slots) {
        stage = Shader::Stage::Compute;
        code = {0x123456789abcdef0, 0xfedcba9876543210};
        cached_lowest = 0;
        cached_highest = sizeof(u64);
        workgroup_size = {64, 1, 1};
        shared_memory_size = 0x600;
        cbuf_values.emplace(0, marker);
        for (const PointerSlot& slot : slots) {
            const u64 key = (u64{slot.index} << 32) | slot.offset;
            cbuf_values.emplace(key, slot.low);
            cbuf_values.emplace(key + 4, slot.high);
            storage_pointer_types.emplace(key, static_cast<u8>(slot.mapped));
        }
    }

    void AddRaw(u64 key, u8 type) {
        storage_pointer_types.emplace(key, type);
    }

    u32 ReadCbufValue(u32, u32) override {
        return 0;
    }
    Shader::TextureType ReadTextureType(u32) override {
        return Shader::TextureType::Color2D;
    }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return {};
    }
    bool IsTexturePixelFormatInteger(u32) override {
        return false;
    }
    u32 ReadViewportTransformState() override {
        return 1;
    }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
};

constexpr Vulkan::ComputePipelineCacheKey CullingKey{
    .unique_hash = 0x541ba22d44898196,
    .shared_memory_size = 0x600,
    .workgroup_size = {64, 1, 1},
};

void SerializeCompute(const Vulkan::ComputePipelineCacheKey& key, const PointerEnvironment& env,
                      const std::filesystem::path& path) {
    const std::array<const VideoCommon::GenericEnvironment*, 1> envs{&env};
    VideoCommon::SerializePipeline(key, envs, path, VideoCommon::VulkanPipelineCacheVersion,
                                   ReadProperties);
}

// The order LoadDiskResources tries the records of each compute key in, by marker (c0[0]).
std::unordered_map<u64, std::vector<VideoCommon::FileEnvironment>> LoadCandidates(
    const std::filesystem::path& path, bool by_mapped) {
    std::unordered_map<u64, std::vector<VideoCommon::FileEnvironment>> candidates;
    VideoCommon::LoadPipelines(
        {}, path, VideoCommon::VulkanPipelineCacheVersion,
        [&](std::ifstream& file, VideoCommon::FileEnvironment env) {
            Vulkan::ComputePipelineCacheKey key;
            file.read(reinterpret_cast<char*>(&key), sizeof(key));
            candidates[key.unique_hash].push_back(std::move(env));
        },
        [](std::ifstream&, std::vector<VideoCommon::FileEnvironment>) {
            FAIL("Unexpected graphics record");
        });
    for (auto& [key, envs] : candidates) {
        Vulkan::OrderComputeTranslations(envs, by_mapped, true);
    }
    return candidates;
}

std::vector<u32> Markers(std::vector<VideoCommon::FileEnvironment>& envs) {
    std::vector<u32> markers;
    for (auto& env : envs) {
        markers.push_back(env.ReadCbufValue(0, 0));
    }
    return markers;
}

} // Anonymous namespace

TEST_CASE("Cached compute records of one key are tried with the most bound pointers first",
          "[pipeline_cache_files][compute_null_slots]") {
    TempDir temp;
    const auto path = temp.path / "compute.bin";
    // 'A' was translated while c4[0x90] was null; 'B' and 'C' with it filled (a rebuild appends
    // its record after the stale one); 'D' is another key.
    const PointerEnvironment a{'A', {{4, 0x80, 0x1000, 0x22, true}, {4, 0x90, 0, 0, false}}};
    const PointerEnvironment b{'B', {{4, 0x80, 0x1000, 0x22, true}, {4, 0x90, 0x2000, 0x22, true}}};
    const PointerEnvironment c{'C', {{4, 0x80, 0x1000, 0x22, true}, {4, 0x90, 0x3000, 0x22, true}}};
    PointerEnvironment d{'D', {{4, 0x80, 0x1000, 0x22, false}}};
    d.AddRaw((u64{1} << 63) | (u64{8} << 40) | (u64{4} << 32) | 0x90, 0); // pointer-table entry
    d.AddRaw((u64{4} << 32) | 0x130, 0); // classified without its words recorded
    auto other = CullingKey;
    other.unique_hash = 0x59c1699f529e7c08;
    SerializeCompute(CullingKey, a, path);
    SerializeCompute(CullingKey, b, path);
    SerializeCompute(other, d, path);
    SerializeCompute(CullingKey, c, path);

    auto mapped_first = LoadCandidates(path, true);
    REQUIRE(mapped_first.size() == 2);
    auto& culling = mapped_first[CullingKey.unique_hash];
    // More bound pointers first; on a tie the earlier record (B before C); then the stale one.
    CHECK(Markers(culling) == std::vector<u32>{'B', 'C', 'A'});
    CHECK(culling[0].MappedStoragePointerKeys().size() == 2);
    CHECK(culling[2].MappedStoragePointerKeys().size() == 1);
    CHECK(culling[0].NullStoragePointerSlots().empty());
    CHECK(culling[2].NullStoragePointerSlots() == std::vector<std::pair<u32, u32>>{{4, 0x90}});
    // A non-null unmapped value, a pointer-table entry and a slot without its words are not null
    // slots: nothing proves the translation saw a null pointer there.
    auto& lone = mapped_first[other.unique_hash];
    REQUIRE(lone.size() == 1);
    CHECK(lone[0].MappedStoragePointerKeys().size() == 0);
    CHECK(lone[0].NullStoragePointerSlots().empty());

    // Switched off: file order, as the loader read records before.
    auto file_order = LoadCandidates(path, false);
    CHECK(Markers(file_order[CullingKey.unique_hash]) == std::vector<u32>{'A', 'B', 'C'});
}

TEST_CASE("A translation depends only on the null slots it queried",
          "[pipeline_cache_files][compute_null_slots]") {
    using VideoCommon::TranslationCache::Query;
    using VideoCommon::TranslationCache::QueryKind;
    const std::vector<std::pair<u32, u32>> slots{{4, 0x90}, {4, 0x130}, {3, 0x660}, {4, 0x170}};
    const std::vector<Query> queries{
        {.kind = QueryKind::CbufValue, .a = 3, .b = 0x660},
        {.kind = QueryKind::CbufStorageBufferPointer, .a = 4, .b = 0x90, .answer = 0},
        {.kind = QueryKind::CbufIndirectStorageBufferPointer, .a = 4, .b = 0x130, .c = 8},
        {.kind = QueryKind::CbufStorageBufferPointer, .a = 4, .b = 0x170, .answer = 0},
    };
    CHECK(Vulkan::NullSlotsAskedBy(slots, queries) ==
          std::vector<std::pair<u32, u32>>{{4, 0x90}, {4, 0x130}, {4, 0x170}});
    CHECK(Vulkan::NullSlotsAskedBy(slots, {}).empty());
    CHECK(Vulkan::NullSlotsAskedBy({}, queries).empty());
}

TEST_CASE("EDEN_NX2_COMPUTE_NULL_SLOT_REBUILD values", "[pipeline_cache_files][compute_null_slots]") {
    CHECK(Vulkan::ParseComputeNullSlotRebuild(nullptr));
    for (const char* on : {"", "1", "on", " TRUE ", "\t1\n"}) {
        CHECK(Vulkan::ParseComputeNullSlotRebuild(on));
    }
    for (const char* off : {"0", "off", "False", "NONE", " 0\n", "yes", "2", "o n"}) {
        CHECK_FALSE(Vulkan::ParseComputeNullSlotRebuild(off));
    }
}

TEST_CASE("Compute cache ordering prefers supersets over larger disjoint pointer sets",
          "[pipeline_cache_files][compute_null_slots]") {
    TempDir temp;
    const auto path = temp.path / "supersets.bin";
    const PointerEnvironment a{'A', {{4, 0x90, 0x1000, 0, true}}};
    const PointerEnvironment b{'B', {{4, 0x130, 0x2000, 0, true},
                                      {4, 0x170, 0x3000, 0, true},
                                      {4, 0x1b0, 0x4000, 0, true}}};
    const PointerEnvironment c{'C', {{4, 0x90, 0x1000, 0, true},
                                      {4, 0x130, 0x2000, 0, true}}};
    SerializeCompute(CullingKey, a, path);
    SerializeCompute(CullingKey, b, path);
    SerializeCompute(CullingKey, c, path);
    auto ordered = LoadCandidates(path, true);
    CHECK(Markers(ordered[CullingKey.unique_hash]) == std::vector<u32>{'C', 'A', 'B'});
    auto count_only = LoadCandidates(path, false);
    Vulkan::OrderComputeTranslations(count_only[CullingKey.unique_hash], true, false);
    CHECK(Markers(count_only[CullingKey.unique_hash]) == std::vector<u32>{'B', 'C', 'A'});
}

TEST_CASE("A chain of null-slot rebuild records loads the newest, strongest one first",
          "[pipeline_cache_files][compute_null_slots]") {
    // Daniel's live cache after the 2026-11-07 20:01 session holds three records of the far-tree
    // culling compute 541ba22d44898196: the product's (c4[0xd0], c4[0x130] and c4[0x170] null),
    // the first rebuild (c4[0xd0] and c4[0x170] bound) and the second (c4[0x130] too), appended
    // in that order with no replacement mark. With retained pointers each rebuild binds every
    // pointer the one before it binds, so the last is the superset with the most pointers: the
    // loader tries it first whichever order the records sit in, and the stale slot that made a
    // session rebuild is no longer null in what boot loads.
    const PointerEnvironment stale{'S', {{4, 0x80, 0x1000, 0x22, true},
                                         {4, 0xd0, 0, 0, false},
                                         {4, 0x130, 0, 0, false},
                                         {4, 0x170, 0, 0, false}}};
    const PointerEnvironment first{'1', {{4, 0x80, 0x1000, 0x22, true},
                                         {4, 0xd0, 0x2000, 0x22, true},
                                         {4, 0x130, 0, 0, false},
                                         {4, 0x170, 0x3000, 0x22, true}}};
    const PointerEnvironment second{'2', {{4, 0x80, 0x1000, 0x22, true},
                                          {4, 0xd0, 0x2000, 0x22, true},
                                          {4, 0x130, 0x4000, 0x22, true},
                                          {4, 0x170, 0x3000, 0x22, true}}};
    TempDir temp;
    // Live order (chronological) and a newest-first product holding every record.
    const auto live = temp.path / "live.bin";
    SerializeCompute(CullingKey, stale, live);
    SerializeCompute(CullingKey, first, live);
    SerializeCompute(CullingKey, second, live);
    const auto newest_first = temp.path / "newest-first.bin";
    SerializeCompute(CullingKey, second, newest_first);
    SerializeCompute(CullingKey, first, newest_first);
    SerializeCompute(CullingKey, stale, newest_first);
    for (const auto& path : {live, newest_first}) {
        CAPTURE(path.filename().string());
        auto candidates = LoadCandidates(path, true);
        auto& culling = candidates[CullingKey.unique_hash];
        REQUIRE(culling.size() == 3);
        CHECK(culling.front().ReadCbufValue(0, 0) == '2');
        CHECK(culling.front().NullStoragePointerSlots().empty());
        CHECK(culling.front().MappedStoragePointerKeys().size() == 4);
    }
    // A product deduplicated to the first rebuild alone (a union that keeps the first record of
    // a key, fed the appended records oldest first) still leaves c4[0x130] null: boot loads it
    // and the area that fills c4[0x130] rebuilds the pipeline again, cold.
    const auto first_only = temp.path / "first-only.bin";
    SerializeCompute(CullingKey, first, first_only);
    auto candidates = LoadCandidates(first_only, true);
    CHECK(candidates[CullingKey.unique_hash].front().NullStoragePointerSlots() ==
          std::vector<std::pair<u32, u32>>{{4, 0x130}});
}

TEST_CASE("Compute rebuild keeps direct and table pointer classifications through serialization",
          "[pipeline_cache_files][compute_null_slots]") {
    TempDir temp;
    const auto path = temp.path / "kept.bin";
    PointerEnvironment env{'A', {{4, 0x90, 0, 0, false}, {4, 0x130, 0, 0, false}}};
    const u64 direct = (u64{4} << 32) | 0x90;
    const u64 table = (u64{1} << 63) | (u64{8} << 40) | (u64{4} << 32) | 0x130;
    env.AddRaw(table, 0);
    const std::array kept{direct, table};
    env.KeepStoragePointers(kept);
    CHECK(env.IsCbufStorageBufferPointer(4, 0x90));
    CHECK(env.IsCbufIndirectStorageBufferPointer(4, 0x130, 8));
    CHECK(env.NullStoragePointerSlots() == std::vector<std::pair<u32, u32>>{{4, 0x130}});
    SerializeCompute(CullingKey, env, path);
    auto candidates = LoadCandidates(path, false);
    auto& loaded = candidates[CullingKey.unique_hash].front();
    CHECK(loaded.MappedStoragePointerKeys() == std::vector<u64>{direct, table});
    CHECK(loaded.IsCbufStorageBufferPointer(4, 0x90));
    CHECK(loaded.IsCbufIndirectStorageBufferPointer(4, 0x130, 8));
}

TEST_CASE("Cached pointer metadata keeps indirect holders and excludes unrelated replay queries",
          "[pipeline_cache_files][compute_null_slots]") {
    using VideoCommon::TranslationCache::Query;
    using VideoCommon::TranslationCache::QueryKind;
    const u64 holder = (u64{4} << 32) | 0x130;
    const u64 table = (u64{1} << 63) | (u64{8} << 40) | holder;
    const std::vector<Query> queries{
        {.kind = QueryKind::CbufIndirectStorageBufferPointer, .a = 4, .b = 0x130,
         .answer = 1, .c = 8},
    };
    CHECK(Vulkan::MappedPointersAskedBy({holder, holder + 8, table, table + 8}, queries) ==
          std::vector<u64>{holder, table});
    CHECK(Vulkan::NullSlotsAskedBy({{4, 0x130}, {4, 0x138}}, queries) ==
          std::vector<std::pair<u32, u32>>{{4, 0x130}});
}
