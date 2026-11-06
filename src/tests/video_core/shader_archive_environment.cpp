// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <fstream>
#include <nlohmann/json.hpp>
#include "video_core/offline_shader_producer.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "shader_recompiler/exception.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/maxwell/structured_control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "video_core/shader_archive_environment.h"
#include "video_core/shader_translation_cache.h"

namespace {
using VideoCommon::ArchiveEnvironment;
using namespace VideoCommon::TranslationCache;
using Catch::Matchers::ContainsSubstring;

struct SyntheticProgram {
    std::vector<u8> bytes = std::vector<u8>(0x200);
    std::vector<u8> control = std::vector<u8>(0x58);

    static void Put(std::vector<u8>& out, size_t offset, u32 word) {
        for (size_t i = 0; i < 4; ++i) {
            out.at(offset + i) = static_cast<u8>(word >> (i * 8));
        }
    }
    SyntheticProgram() {
        Put(bytes, 0, 1 << 10); // Vertex SPH.
        Put(bytes, 0x80, 0x0000794d); // EXIT PT.
        Put(bytes, 0x88, 0x03800000);
        Put(bytes, 0x100, 0x12345678);
        Put(control, 0, 0x98761234);
        Put(control, 4, 0xc9);
        Put(control, 8, 20);
        Put(control, 0x38, 0x90);
        Put(control, 0x3c, 4);
        Put(control, 0x40, 0x100);
    }
    ArchiveEnvironment Make() const {
        return ArchiveEnvironment(bytes, control, Shader::Stage::VertexB);
    }
};

Shader::ConstBufferAddr Handle(u32 offset = 0x20) {
    Shader::ConstBufferAddr address{};
    address.index = 2;
    address.offset = offset;
    address.count = 1;
    return address;
}
} // namespace

TEST_CASE("Archive environment owns bounded code and exact bank 1", "[video_core][archive_environment]") {
    SyntheticProgram source;
    auto env = source.Make();
    const auto hash = env.Analyze();
    REQUIRE(hash.has_value());
    REQUIRE(env.IsNativeProgram());
    REQUIRE(env.HasNativeInstructions());
    REQUIRE(env.IsProprietaryDriver());
    REQUIRE(env.StartAddress() == 0);
    REQUIRE(env.ReadCbufValue(1, 0) == 0x12345678);
    REQUIRE(env.TranslateProgramAddress(0) == sizeof(Shader::ProgramHeader));
    REQUIRE_NOTHROW(env.ReadInstruction(sizeof(Shader::ProgramHeader)));
    REQUIRE_THROWS(env.ReadInstruction(0x100000));
    source.bytes.clear();
    source.control.clear();
    REQUIRE(env.ReadCbufValue(1, 0) == 0x12345678);
    REQUIRE(env.Analyze() == hash);
    REQUIRE_THROWS_WITH(env.ReadCbufValue(1, 1), ContainsSubstring("missing_constant"));
    REQUIRE_THROWS_WITH(env.ReadCbufValue(2, 0), ContainsSubstring("missing_constant"));
    REQUIRE_THROWS_WITH(env.ReadCbufValue(1, ~0U), ContainsSubstring("missing_constant"));
}

TEST_CASE("Archive guesses cannot escape into exact reads", "[video_core][archive_environment]") {
    auto env = SyntheticProgram{}.Make();
    RecordingEnvironment recording(env, true);
    recording.BeginTextureHandle();
    const u32 first = recording.ReadCbufValue(2, 0x20);
    REQUIRE(recording.ReadCbufValue(2, 0x20) == first);
    REQUIRE(recording.ReadCbufValue(2, 0x28) != first);
    recording.EndTextureHandle(Handle(), first);
    REQUIRE(recording.ReadTextureType(first) == Shader::TextureType::Color2D);
    REQUIRE(recording.ReadTexturePixelFormat(first) == Shader::TexturePixelFormat::A8B8G8R8_UNORM);
    REQUIRE_FALSE(recording.IsTexturePixelFormatInteger(first));
    REQUIRE(recording.IsCbufStorageBufferPointer(2, 0));
    REQUIRE_FALSE(recording.IsCbufIndirectStorageBufferPointer(2, 0, 0));
    REQUIRE(recording.ReadViewportTransformState() == 1);
    REQUIRE_FALSE(recording.GetReplaceConstBuffer(2, 0).has_value());
    const auto queries = recording.TakeQueries();
    REQUIRE(queries.front().kind == QueryKind::TextureHandle);
    REQUIRE_THROWS_WITH(env.ReadCbufValue(2, 0x20), ContainsSubstring("missing_constant"));
    REQUIRE_THROWS_WITH(env.NoteExactCbufWord(2, 0x20), ContainsSubstring("missing_constant"));
}

TEST_CASE("Archive environment rejects malformed ranges and unsupported recipes", "[video_core][archive_environment]") {
    SyntheticProgram source;
    SECTION("constant offset overflow") { SyntheticProgram::Put(source.control, 0x40, ~0U); }
    SECTION("constant section overlaps code") { SyntheticProgram::Put(source.control, 0x40, 0x80); }
    SECTION("constant section exceeds program") { source.bytes.resize(0x102); }
    SECTION("missing control") { source.control.clear(); }
    SECTION("unknown control version") { SyntheticProgram::Put(source.control, 8, 21); }
    SECTION("incomplete capture") { SyntheticProgram::Put(source.bytes, 0x80, 0x00007918); }
    SECTION("trap guard") { SyntheticProgram::Put(source.bytes, 0x80, 0x0000795c); }
    SECTION("wrong stage header") { SyntheticProgram::Put(source.bytes, 0, 5 << 10); }
    SECTION("wrong control stage") { source.control[0x56] = 1; }
    REQUIRE_THROWS(source.Make());
}

TEST_CASE("Archive environment excludes address-dependent geometry", "[video_core][archive_environment]") {
    SyntheticProgram source;
    SyntheticProgram::Put(source.bytes, 0, 4 << 10);
    SyntheticProgram::Put(source.bytes, 0x80, 0x00007918); // NOP, not an exit envelope.
    SyntheticProgram::Put(source.bytes, 0x88, 0);
    SyntheticProgram::Put(source.bytes, 0x90, 0x0000734e); // PC -> R0/R1.
    SyntheticProgram::Put(source.bytes, 0xa0, 0x0000794d);
    SyntheticProgram::Put(source.bytes, 0xa8, 0x03800000);
    SyntheticProgram::Put(source.control, 0x38, 0xb0);
    REQUIRE_THROWS_WITH(ArchiveEnvironment(source.bytes, source.control, Shader::Stage::Geometry),
                        ContainsSubstring("base_dependent"));
    REQUIRE_THROWS_WITH(ArchiveEnvironment(source.bytes, source.control, Shader::Stage::Compute),
                        ContainsSubstring("unsupported_stage"));
    SyntheticProgram::Put(source.bytes, 0x90, 0x00ff734e); // Discarded PC load to RZ.
    REQUIRE_NOTHROW(ArchiveEnvironment(source.bytes, source.control, Shader::Stage::Geometry));
}

TEST_CASE("Archive program emits an ordinary translation cache variant", "[video_core][archive_environment]") {
    auto env = SyntheticProgram{}.Make();
    RecordingEnvironment recording(env, true);
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_pool;
    Shader::Maxwell::Flow::CFG cfg{recording, flow_pool, Shader::Maxwell::Location{0x50}};
    Shader::ObjectPool<Shader::IR::Inst> inst_pool;
    Shader::ObjectPool<Shader::IR::Block> block_pool;
    auto program = Shader::Maxwell::TranslateProgram(inst_pool, block_pool, recording, cfg,
                                                     Shader::HostTranslateInfo{});
    const auto meta = ProgramMeta::From(program);
    Shader::Profile profile{};
    profile.supported_spirv = 0x00010300;
    Shader::RuntimeInfo runtime{};
    Shader::Backend::Bindings bindings{};
    const auto incoming = bindings;
    const auto code = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime, program, bindings);
    REQUIRE_FALSE(code.empty());
    REQUIRE(code.front() == 0x07230203);
    const Digest config = MakeConfigDigest(profile, {}, 0);
    const auto key = MakeStageKey(env, *env.Analyze(), 1, config);
    Store store;
    store.Open({}, config);
    const auto entry = store.Insert(key, recording.TakeQueries(), meta);
    REQUIRE(entry);
    const auto variant_key = MakeVariantKey(entry->id, DigestRuntimeInfo(runtime), incoming);
    store.InsertVariant(*entry, variant_key, {bindings, program.info, code});
    REQUIRE(store.FindEntry(key, env));
    const auto variant = store.FindVariant(variant_key);
    REQUIRE(variant.has_value());
    REQUIRE(variant->spirv == code);
    REQUIRE(InfoEqual(variant->info, program.info));
}

TEST_CASE("Offline producer resumes complete modules and repairs interrupted appends",
          "[video_core][translation_cache][archive_environment]") {
    namespace Offline = VideoCommon::OfflineShaders;
    const auto directory = std::filesystem::temp_directory_path() / "lichii_offline_producer_test";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    SyntheticProgram source;
    auto write = [&](const char* name, const std::vector<u8>& bytes) {
        std::ofstream file(directory / name, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    };
    write("code.bin", source.bytes);
    write("control.bin", source.control);
    u64 fingerprint = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < 256; ++i) fingerprint = (fingerprint ^ source.bytes[i]) * 0x100000001b3ULL;
    nlohmann::json row{{"container", "code.bin"}, {"control", "control.bin"},
                       {"offset", 0}, {"size", source.bytes.size()}, {"control_offset", 0},
                       {"constants_offset", 256}, {"constants_size", 4},
                       {"stage", "vertex"}, {"fingerprint", fmt::format("{:016x}", fingerprint)},
                       {"bank1_words", {0x12345678}}};
    nlohmann::json index{{"schema_version", 1}, {"title_id", "0400000000000000"},
                         {"programs", {row, row}}};
    const auto input = directory / "index.json";
    {
        std::ofstream file(input);
        file << index;
    }
    const auto path = directory / "cache.bin";
    Shader::Profile profile{};
    profile.supported_spirv = 0x00010300;
    const auto config = MakeConfigDigest(profile, {}, 0);
    size_t metadata_length{};
    std::vector<u8> complete;
    {
        Store store;
        store.Open(path, config);
        const auto first = Offline::Run(input, 0x0400000000000000ULL, 2, {}, profile, {}, config, store);
        REQUIRE(first.translated == 1);
        REQUIRE(first.resumed == 1);
        REQUIRE(first.modules == 1);
        REQUIRE(first.skipped.empty());
        REQUIRE_THROWS(Offline::Run(input, 1, 2, {}, profile, {}, config, store));
        const auto size = std::filesystem::file_size(path);
        const auto second = Offline::Run(input, 0x0400000000000000ULL, 2, path, profile, {}, config, store);
        REQUIRE(second.translated == 0);
        REQUIRE(second.resumed == 2);
        REQUIRE(std::filesystem::file_size(path) == size);
        {
            Offline::SignalGuard signals;
            std::raise(SIGTERM);
            const auto cancelled = Offline::Run(input, 0x0400000000000000ULL, 2, path,
                                                 profile, {}, config, store);
            REQUIRE(cancelled.cancelled);
            REQUIRE(cancelled.translated == 0);
            REQUIRE(cancelled.resumed == 0);
            REQUIRE(cancelled.modules == 0);
            REQUIRE(std::filesystem::file_size(path) == size);
        }
        Offline::SignalGuard reset;
        std::ifstream file(path, std::ios::binary);
        complete.assign(std::istreambuf_iterator<char>(file), {});
        REQUIRE(complete.size() > 36);
        u32 record_length{};
        std::memcpy(&record_length, complete.data() + 32, 4);
        metadata_length = 36 + record_length;
    }
    // Cut every byte of the first appended record, then verify that resumption emits the module.
    for (size_t length = metadata_length; length < complete.size(); ++length) {
        write("cache.bin", std::vector<u8>(complete.begin(), complete.begin() + length));
        Store store;
        store.Open(path, config);
        const auto result = Offline::Run(input, 0x0400000000000000ULL, 1, {}, profile, {}, config, store);
        REQUIRE(result.modules == 1);
        REQUIRE(result.resumed == 1);
        REQUIRE(result.skipped.empty());
    }
    // Exact-stage recipes survive source configuration changes and each gets a fresh IR emission.
    const auto recipes_path = directory / "recipes.bin";
    const Digest source_config{123, 456};
    {
        auto env = source.Make();
        Store recipes;
        recipes.Open(recipes_path, source_config);
        const auto key = MakeStageKey(env, *env.Analyze(), 1, source_config);
        ProgramMeta meta{};
        meta.stage = Shader::Stage::VertexB;
        const auto entry = recipes.Insert(key, {}, meta);
        for (u32 texture : {2, 3, 4}) {
            auto recipe = DefaultEmissionRecipe(meta.stage);
            recipe.bindings.texture = texture;
            REQUIRE(recipes.InsertVariant(*entry,
                MakeVariantKey(entry->id, DigestRuntimeInfo(recipe.runtime), recipe.bindings),
                {.spirv = {0x07230203, 0x10000, 0, 1, 0}}, &recipe));
        }
    }
    {
        Store store;
        store.Open(directory / "multi.bin", config);
        const auto first = Offline::Run(input, 0x0400000000000000ULL, 2, recipes_path,
                                        profile, {}, config, store, 1);
        REQUIRE(first.modules == 2);
        REQUIRE(first.translated == 2);
        REQUIRE(first.module_bytes >= first.modules * 20);
        REQUIRE(first.skipped.empty());
        const auto size = std::filesystem::file_size(directory / "multi.bin");
        const auto second = Offline::Run(input, 0x0400000000000000ULL, 2, recipes_path,
                                         profile, {}, config, store, 1);
        REQUIRE(second.modules == 0);
        REQUIRE(second.translated == 0);
        REQUIRE(second.resumed == 2);
        REQUIRE(std::filesystem::file_size(directory / "multi.bin") == size);
    }
    std::filesystem::remove_all(directory);
}

namespace {
// Mirrors GraphicsEnvironment's native, non-mesh draw inputs without a GPU or guest memory.
class NativeDrawEnvironment final : public VideoCommon::GenericEnvironment {
public:
    NativeDrawEnvironment(const SyntheticProgram& source,
                          std::shared_ptr<const VideoCommon::LoweredNativeCode> lowered) {
        std::memcpy(&sph, source.bytes.data(), sizeof(sph));
        stage = Shader::Stage::VertexB;
        initial_offset = sizeof(sph);
        texture_bound = 0;
        InitializeNativeCode(std::move(lowered), 128, false);
        local_memory_size = static_cast<u32>(sph.LocalMemorySize()) +
                            sph.common3.shader_local_memory_crs_size;
        is_proprietary_driver = true;
    }
    u32 ReadCbufValue(u32 index, u32 offset) override {
        REQUIRE(index == 1);
        REQUIRE(offset == 0);
        return 0x12345678;
    }
    Shader::TextureType ReadTextureType(u32) override { return Shader::TextureType::Color2D; }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return Shader::TexturePixelFormat::A8B8G8R8_UNORM;
    }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 1; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
};
} // namespace

TEST_CASE("Archive and native draw inputs yield identical keys and serialized metadata",
          "[video_core][translation_cache][archive_environment]") {
    SyntheticProgram source;
    auto archive = source.Make();
    NativeDrawEnvironment live(source, archive.LoweredNative());
    const Digest config{11, 22};
    REQUIRE(archive.Analyze() == live.Analyze());
    REQUIRE(MakeStageKey(archive, *archive.Analyze(), 1, config) ==
            MakeStageKey(live, *live.Analyze(), 1, config));
    const auto translate = [](Shader::Environment& env) {
        Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow;
        Shader::ObjectPool<Shader::IR::Inst> inst;
        Shader::ObjectPool<Shader::IR::Block> blocks;
        Shader::Maxwell::Flow::CFG cfg(env, flow, Shader::Maxwell::Location{0x50});
        auto program = Shader::Maxwell::TranslateProgram(inst, blocks, env, cfg, {});
        std::vector<u8> bytes;
        WriteInfo(bytes, program.info);
        return bytes;
    };
    RecordingEnvironment recording(archive, true);
    REQUIRE(translate(recording) == translate(live));
    REQUIRE(ReplayQueries(recording.TakeQueries(), live));
}

TEST_CASE("Info serialization excludes descriptor padding and preserves real differences",
          "[video_core][translation_cache][archive_environment]") {
    Shader::Info first{}, second{};
    Shader::StorageBufferDescriptor descriptor{};
    descriptor.cbuf_index = 0;
    descriptor.cbuf_offset = 0x570;
    descriptor.count = 1;
    first.storage_buffers_descriptors.push_back(descriptor);
    second.storage_buffers_descriptors.push_back(descriptor);
    auto& left = first.storage_buffers_descriptors.front();
    auto& right = second.storage_buffers_descriptors.front();
    const size_t begin = offsetof(Shader::StorageWriteExtent, guarded) + sizeof(bool);
    const size_t end = offsetof(Shader::StorageWriteExtent, const_end);
    REQUIRE(end > begin);
    for (size_t i = begin; i < end; ++i) {
        reinterpret_cast<u8*>(&left.write_extent)[i] = 0x34;
        reinterpret_cast<u8*>(&right.write_extent)[i] = 0xd4;
    }
    REQUIRE(InfoEqual(first, second));
    std::vector<u8> a, b;
    WriteInfo(a, first);
    WriteInfo(b, second);
    REQUIRE(a == b);
    right.write_extent.const_end = 4;
    b.clear();
    WriteInfo(b, second);
    REQUIRE(a != b);
    REQUIRE_FALSE(InfoEqual(first, second));
}

TEST_CASE("Native archive texture bank starts at zero and follows bound instructions",
          "[video_core][translation_cache][archive_environment]") {
    SyntheticProgram source;
    SECTION("no bound texture instruction") {
        const auto env = source.Make();
        REQUIRE(env.TextureBoundBuffer() == 0);
        REQUIRE(env.IsProprietaryDriver());
    }
    SECTION("bound texture LOD query selects bank two") {
        // TMML R0, R0, RZ, descriptor 0 in bank 2, 2D, one component.
        SyntheticProgram::Put(source.bytes, 0x80, 0x00007b69);
        SyntheticProgram::Put(source.bytes, 0x84, 0x208000ff);
        SyntheticProgram::Put(source.bytes, 0x88, 0x000001ff);
        SyntheticProgram::Put(source.bytes, 0x90, 0x0000794d);
        SyntheticProgram::Put(source.bytes, 0x98, 0x03800000);
        SyntheticProgram::Put(source.control, 0x38, 0xa0);
        const auto env = source.Make();
        REQUIRE(env.LoweredNative()->texture_bound == 2);
        REQUIRE(env.TextureBoundBuffer() == 2);
        REQUIRE(env.IsProprietaryDriver());
    }
}
