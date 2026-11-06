// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <catch2/catch_test_macros.hpp>
#include "video_core/shader_translation_cache.h"

namespace {
using namespace VideoCommon::TranslationCache;

class FakeEnvironment final : public Shader::Environment {
public:
    FakeEnvironment() {
        stage = Shader::Stage::Fragment;
        start_address = 0x80;
    }

    u64 ReadInstruction(u32) override {
        return 0;
    }
    u32 ReadCbufValue(u32 index, u32 offset) override {
        ++reads;
        return cbufs[(u64{index} << 32) | offset];
    }
    Shader::TextureType ReadTextureType(u32 handle) override {
        return handle == 7 ? Shader::TextureType::ColorArray2D : Shader::TextureType::Color2D;
    }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return Shader::TexturePixelFormat{};
    }
    bool IsTexturePixelFormatInteger(u32) override {
        return false;
    }
    u32 ReadViewportTransformState() override {
        return viewport;
    }
    u32 TextureBoundBuffer() const override {
        return texture_bound;
    }
    u32 LocalMemorySize() const override {
        return 0;
    }
    u32 SharedMemorySize() const override {
        return 0;
    }
    std::array<u32, 3> WorkgroupSize() const override {
        return {1, 1, 1};
    }
    bool HasHLEMacroState() const override {
        return false;
    }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    bool IsCbufIndirectStorageBufferPointer(u32, u32, u32 indirect) override {
        return indirect_pointers.contains(indirect);
    }
    void Dump(u64, u64) override {}

    void SetHeaderWord(size_t word, u32 value) {
        std::memcpy(reinterpret_cast<u8*>(&sph) + word * sizeof(u32), &value, sizeof(value));
    }

    std::map<u64, u32> cbufs;
    u32 viewport = 1;
    u32 texture_bound = 2;
    u32 reads = 0;
    std::set<u32> indirect_pointers;
};

// What a translation does with its environment, for the record/replay tests: the second read
// depends on the first answer, as a bindless handle lookup does.
void Translate(Shader::Environment& env) {
    const u32 first = env.ReadCbufValue(1, 0x10);
    (void)env.ReadCbufValue(1, first);
    (void)env.ReadTextureType(7);
    (void)env.ReadViewportTransformState();
    (void)env.GetReplaceConstBuffer(0, 4);
}

Shader::TextureDescriptor texture_for_test(u32 index) {
    Shader::TextureDescriptor texture{};
    texture.type = Shader::TextureType::Color2D;
    texture.cbuf_index = 0;
    texture.cbuf_offset = 0x100 + index * 8;
    texture.count = 1;
    return texture;
}

Shader::Info SampleInfo() {
    Shader::Info info{};
    info.uses_sample_id = true;
    info.uses_patches[3] = true;
    info.interpolation[5] = Shader::Interpolation::Flat;
    info.stores.Set(Shader::IR::Attribute::PositionX);
    info.legacy_stores_mapping.emplace(Shader::IR::Attribute::ColorFrontDiffuseR,
                                       Shader::IR::Attribute::Generic0X);
    info.stores_frag_color[2] = true;
    info.constant_buffer_mask = 0x5;
    info.constant_buffer_used_sizes[2] = 0x400;
    info.nvn_buffer_used.set(3);
    info.used_clip_distances = 2;
    info.constant_buffer_descriptors.push_back({.index = 2, .count = 1});
    info.storage_buffers_descriptors.push_back(
        {.cbuf_index = 0, .cbuf_offset = 0x110, .count = 1, .is_written = true});
    Shader::TextureDescriptor texture{};
    texture.type = Shader::TextureType::ColorArray2D;
    texture.cbuf_index = 1;
    texture.cbuf_offset = 0x30;
    texture.count = 1;
    texture.handle_program = 0;
    info.texture_descriptors.push_back(texture);
    info.handle_programs.push_back({1, 2, 3});
    return info;
}

std::filesystem::path TempPath(const char* name) {
    return std::filesystem::temp_directory_path() / name;
}
} // Anonymous namespace

TEST_CASE("Translation cache covers every field of the shader structures",
          "[video_core][translation_cache]") {
    REQUIRE(LayoutIsCovered());
}

TEST_CASE("Translation cache round-trips the shader info", "[video_core][translation_cache]") {
    const Shader::Info info = SampleInfo();
    std::vector<u8> bytes;
    WriteInfo(bytes, info);
    std::span<const u8> in{bytes};
    Shader::Info read{};
    REQUIRE(ReadInfo(in, read));
    REQUIRE(in.empty());
    std::vector<u8> again;
    WriteInfo(again, read);
    REQUIRE(again == bytes);
    REQUIRE(InfoEqual(read, info));
    Shader::Info changed = info;
    changed.texture_descriptors[0].cbuf_offset = 0x34;
    REQUIRE_FALSE(InfoEqual(changed, info));
    changed = info;
    changed.stores.Set(Shader::IR::Attribute::PositionY);
    REQUIRE_FALSE(InfoEqual(changed, info));
    changed = info;
    changed.handle_programs[0].push_back(4);
    REQUIRE_FALSE(InfoEqual(changed, info));
    REQUIRE(read.uses_sample_id);
    REQUIRE(read.legacy_stores_mapping.size() == 1);
    REQUIRE(read.texture_descriptors.size() == 1);
    REQUIRE(read.texture_descriptors[0].cbuf_offset == 0x30);
    REQUIRE(read.handle_programs == std::vector<std::vector<u32>>{{1, 2, 3}});
    REQUIRE(read.storage_buffers_descriptors[0].is_written);

    // More descriptors than a small_vector keeps inline (a shader with 20 textures).
    Shader::Info many = SampleInfo();
    for (u32 i = 0; i < 20; ++i) {
        many.texture_descriptors.push_back(texture_for_test(i));
    }
    std::vector<u8> many_bytes;
    WriteInfo(many_bytes, many);
    std::span<const u8> many_in{many_bytes};
    Shader::Info many_read{};
    REQUIRE(ReadInfo(many_in, many_read));
    REQUIRE(many_read.texture_descriptors.size() == 21);
    REQUIRE(many_read.texture_descriptors[20].cbuf_offset == 0x100 + 19 * 8);

    // Truncated input is rejected rather than read past its end.
    std::span<const u8> cut{bytes.data(), bytes.size() - 3};
    REQUIRE_FALSE(ReadInfo(cut, read));
}

TEST_CASE("Translation cache replays the queries a translation made",
          "[video_core][translation_cache]") {
    FakeEnvironment env;
    env.cbufs[(u64{1} << 32) | 0x10] = 0x40;
    env.cbufs[(u64{1} << 32) | 0x40] = 0x1234;
    RecordingEnvironment recording{env};
    REQUIRE(recording.ShaderStage() == Shader::Stage::Fragment);
    REQUIRE(recording.StartAddress() == 0x80);
    Translate(recording);
    const std::vector<Query> queries = recording.TakeQueries();
    REQUIRE(queries.size() == 5);
    REQUIRE(ReplayQueries(queries, env));

    // A changed indirect word: the second query no longer matches.
    env.cbufs[(u64{1} << 32) | 0x40] = 0x9999;
    REQUIRE_FALSE(ReplayQueries(queries, env));
    env.cbufs[(u64{1} << 32) | 0x40] = 0x1234;

    // A changed first word stops the replay at once: the dependent read is never made.
    env.cbufs[(u64{1} << 32) | 0x10] = 0x44;
    env.reads = 0;
    REQUIRE_FALSE(ReplayQueries(queries, env));
    REQUIRE(env.reads == 1);
    env.cbufs[(u64{1} << 32) | 0x10] = 0x40;

    env.viewport = 0;
    REQUIRE_FALSE(ReplayQueries(queries, env));
}

TEST_CASE("Translation cache stage keys cover the environment", "[video_core][translation_cache]") {
    const Digest config{1, 2};
    FakeEnvironment env;
    const Digest key = MakeStageKey(env, 0xabc, 5, config);
    REQUIRE(MakeStageKey(env, 0xabc, 5, config) == key);
    REQUIRE_FALSE(MakeStageKey(env, 0xabd, 5, config) == key);
    REQUIRE_FALSE(MakeStageKey(env, 0xabc, 4, config) == key);
    REQUIRE_FALSE(MakeStageKey(env, 0xabc, 5, Digest{1, 3}) == key);
    env.texture_bound = 3;
    REQUIRE_FALSE(MakeStageKey(env, 0xabc, 5, config) == key);
    env.texture_bound = 2;
    env.SetHeaderWord(4, 0x10);
    REQUIRE_FALSE(MakeStageKey(env, 0xabc, 5, config) == key);
}

TEST_CASE("Translation cache runtime info digest covers emitted state",
          "[video_core][translation_cache]") {
    Shader::RuntimeInfo info{};
    const Digest base = DigestRuntimeInfo(info);
    REQUIRE(DigestRuntimeInfo(info) == base);
    info.alpha_test_func = Shader::CompareFunction::Greater;
    const Digest alpha = DigestRuntimeInfo(info);
    REQUIRE_FALSE(alpha == base);
    info.alpha_test_reference = 0.5f;
    REQUIRE_FALSE(DigestRuntimeInfo(info) == alpha);
    info = {};
    info.color_output_types[1] = Shader::AttributeType::UnsignedInt;
    REQUIRE_FALSE(DigestRuntimeInfo(info) == base);
    info = {};
    info.previous_stage_stores.Set(Shader::IR::Attribute::Generic3X);
    REQUIRE_FALSE(DigestRuntimeInfo(info) == base);
}

TEST_CASE("Translation cache persists entries and variants", "[video_core][translation_cache]") {
    const auto path = TempPath("lichii_translation_cache_test.bin");
    std::filesystem::remove(path);
    const Digest config{0x11, 0x22};
    FakeEnvironment env;
    env.cbufs[(u64{1} << 32) | 0x10] = 0x40;
    const Digest stage_key = MakeStageKey(env, 0x55, 5, config);

    ProgramMeta meta{};
    meta.stage = Shader::Stage::Fragment;
    meta.invocations = 3;
    meta.info = SampleInfo();
    const Shader::Backend::Bindings binding_in{.texture = 2};
    const Digest runtime = DigestRuntimeInfo(Shader::RuntimeInfo{});
    Variant variant{.binding_out = {.unified = 4, .texture = 3}, .info = SampleInfo()};
    variant.info.stores_frag_depth = true;
    for (u32 i = 0; i < 300; ++i) {
        variant.spirv.push_back(0x07230203 + (i % 7));
    }
    Digest variant_key{};
    {
        Store store;
        store.Open(path, config);
        REQUIRE(store.Enabled());
        RecordingEnvironment recording{env};
        Translate(recording);
        const auto entry = store.Insert(stage_key, recording.TakeQueries(), meta);
        REQUIRE(entry);
        variant_key = MakeVariantKey(entry->id, runtime, binding_in);
        store.InsertVariant(*entry, variant_key, variant);
    }
    {
        Store store;
        store.Open(path, config);
        const auto entry = store.FindEntry(stage_key, env);
        REQUIRE(entry);
        REQUIRE(entry->meta.invocations == 3);
        REQUIRE(entry->meta.info.texture_descriptors.size() == 1);
        REQUIRE(MakeVariantKey(entry->id, runtime, binding_in) == variant_key);
        const auto found = store.FindVariant(variant_key);
        REQUIRE(found);
        REQUIRE(found->spirv == variant.spirv);
        REQUIRE(found->binding_out.texture == 3);
        REQUIRE(found->info.stores_frag_depth);
        REQUIRE_FALSE(store.FindVariant(MakeVariantKey(entry->id, runtime, {})));

        // The environment answers a query differently: no entry.
        env.viewport = 0;
        REQUIRE_FALSE(store.FindEntry(stage_key, env));
        env.viewport = 1;
    }
    // A record cut short at the end of the file is dropped; the ones before it are kept.
    {
        std::ofstream file{path, std::ios::binary | std::ios::app};
        const u32 partial[] = {0x52435453, 1000, 7};
        file.write(reinterpret_cast<const char*>(partial), sizeof(partial));
    }
    {
        Store store;
        store.Open(path, config);
        REQUIRE(store.FindEntry(stage_key, env));
        REQUIRE(store.FindVariant(variant_key));
    }
    // Another build or configuration starts over.
    {
        Store store;
        store.Open(path, Digest{0x11, 0x23});
        REQUIRE(store.Enabled());
        REQUIRE_FALSE(store.FindEntry(stage_key, env));
        REQUIRE_FALSE(store.FindVariant(variant_key));
    }
    std::filesystem::remove(path);
}

TEST_CASE("Translation cache persists the mesh info of task and mesh entries",
          "[video_core][translation_cache]") {
    const auto path = TempPath("lichii_translation_cache_mesh_test.bin");
    std::filesystem::remove(path);
    const Digest config{0x31, 0x32};
    FakeEnvironment env;
    const Digest stage_key = MakeStageKey(env, 0x77, 3, config);

    ProgramMeta meta{};
    meta.stage = Shader::Stage::Mesh;
    meta.workgroup_size = {32, 1, 1};
    meta.info = SampleInfo();
    meta.info.is_mesh = true;
    meta.mesh.payload_words = 64;
    meta.mesh.max_vertices = 64;
    meta.mesh.max_primitives = 126;
    meta.mesh.topology = 4;
    for (u32 i = 0; i < 20; ++i) {
        meta.mesh.vertex_lines.push_back(100 + i);
    }
    meta.mesh.primitive_lines.push_back(7);
    {
        Store store;
        store.Open(path, config);
        RecordingEnvironment recording{env};
        Translate(recording);
        REQUIRE(store.Insert(stage_key, recording.TakeQueries(), meta));
    }
    {
        Store store;
        store.Open(path, config);
        const auto entry = store.FindEntry(stage_key, env);
        REQUIRE(entry);
        REQUIRE(entry->meta.info.is_mesh);
        const auto program = entry->meta.ToProgram();
        REQUIRE(program.mesh.payload_words == 64);
        REQUIRE(program.mesh.max_vertices == 64);
        REQUIRE(program.mesh.max_primitives == 126);
        REQUIRE(program.mesh.topology == 4);
        REQUIRE(std::ranges::equal(program.mesh.vertex_lines, meta.mesh.vertex_lines));
        REQUIRE(std::ranges::equal(program.mesh.primitive_lines, meta.mesh.primitive_lines));
    }
    std::filesystem::remove(path);
}

TEST_CASE("Translation cache records indirect storage pointer classifications",
          "[video_core][translation_cache]") {
    FakeEnvironment env;
    env.indirect_pointers.insert(0x18);
    RecordingEnvironment recording{env};
    // The recording environment forwards the live answer instead of the base class's `true`.
    REQUIRE(recording.IsCbufIndirectStorageBufferPointer(2, 0x20, 0x18));
    REQUIRE_FALSE(recording.IsCbufIndirectStorageBufferPointer(2, 0x20, 0x08));
    const std::vector<Query> queries = recording.TakeQueries();
    REQUIRE(queries.size() == 2);
    REQUIRE(queries[0].kind == QueryKind::CbufIndirectStorageBufferPointer);
    REQUIRE(queries[0].c == 0x18);
    REQUIRE(ReplayQueries(queries, env));

    // A pointer table that now classifies differently no longer matches.
    env.indirect_pointers.insert(0x08);
    REQUIRE_FALSE(ReplayQueries(queries, env));
}
