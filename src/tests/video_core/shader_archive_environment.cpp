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
    std::filesystem::remove_all(directory);
}
