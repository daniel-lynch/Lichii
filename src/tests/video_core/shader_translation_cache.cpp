// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <catch2/catch_test_macros.hpp>
#include "common/shader_aot_scene.h"
#include "video_core/shader_pool_index.h"
#include "video_core/shader_recipe_histogram.h"
#include "shader_recompiler/translator_identity.h"
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

TEST_CASE("Translation cache refuses publications from before a reopen",
          "[video_core][translation_cache]") {
    const Digest config{0x41, 0x42};
    FakeEnvironment env;
    const Digest stage_key = MakeStageKey(env, 0x99, 1, config);
    ProgramMeta meta{};
    meta.stage = Shader::Stage::VertexB;
    meta.info = SampleInfo();
    Store store;
    store.Open({}, config);
    REQUIRE(store.Enabled());
    const u64 generation = store.Generation();
    RecordingEnvironment recording{env};
    Translate(recording);
    const auto queries = recording.TakeQueries();
    const auto entry = store.Insert(stage_key, queries, meta, generation);
    REQUIRE(entry);
    const Variant variant{.info = SampleInfo(), .spirv = {0x07230203}};
    REQUIRE(store.InsertVariant(*entry, Digest{1, 1}, variant));
    // An entry that is not the store's own is refused.
    Entry copy = *entry;
    REQUIRE_FALSE(store.InsertVariant(copy, Digest{2, 2}, variant));

    store.Open({}, config);
    REQUIRE(store.Generation() != generation);
    REQUIRE_FALSE(store.Insert(stage_key, queries, meta, generation));
    REQUIRE_FALSE(store.InsertVariant(*entry, Digest{3, 3}, variant));
    REQUIRE_FALSE(store.FindVariant(Digest{3, 3}));
    REQUIRE(store.Insert(stage_key, queries, meta, store.Generation()));
}

TEST_CASE("AOT pool index parses the index file and fingerprints like the tool",
          "[video_core][translation_cache]") {
    using VideoCommon::ShaderPoolIndex;
    // FNV-1a 64 test vectors (tools/oot/container/aot_index.py uses the same function).
    REQUIRE(ShaderPoolIndex::Fingerprint({}) == 0xcbf29ce484222325ULL);
    const std::array<u8, 1> a{'a'};
    REQUIRE(ShaderPoolIndex::Fingerprint(a) == 0xaf63dc4c8601ec8cULL);

    std::vector<u8> data{'L', 'X', 'A', 'O', 'T', 'I', 'X', '2'};
    const auto put = [&](auto value) {
        const auto* bytes = reinterpret_cast<const u8*>(&value);
        data.insert(data.end(), bytes, bytes + sizeof(value));
    };
    put(u32{1});
    put(u32{2});
    put(u32{0x100});
    put(u32{0x380});
    put(u8{4});
    put(u64{0x1122334455667788ULL});
    put(u32{0x300});
    put(u16{0x40});
    put(u32{0x480});
    put(u32{0x80});
    put(u8{0});
    put(u64{7});
    put(u32{~0U});
    put(u16{0});
    std::vector<ShaderPoolIndex::Container> containers;
    REQUIRE(ShaderPoolIndex::Parse(data, containers));
    REQUIRE(containers.size() == 1);
    REQUIRE(containers[0].programs.size() == 2);
    REQUIRE(containers[0].programs[0].offset == 0x100);
    REQUIRE(containers[0].programs[0].stage == 4);
    REQUIRE(containers[0].programs[1].fingerprint == 7);
    REQUIRE(containers[0].programs[0].constants_offset == 0x300);
    REQUIRE(containers[0].programs[0].constants_size == 0x40);
    data.pop_back();
    REQUIRE_FALSE(ShaderPoolIndex::Parse(data, containers));
}

TEST_CASE("AOT scene manifest marks indexed programs by tier and keeps the bank-1 words",
          "[video_core][translation_cache]") {
    using VideoCommon::ShaderPoolIndex;
    // Rows as build_manifest.py writes them (made-up values, not game data).
    const std::string_view json = R"({"scene": "Test", "schema": "oot-aot-static-manifest-1",
      "programs": [
        {"fingerprint": "00000000000000aa", "pool_offset": 256, "size": 896, "stage": "fragment",
         "tier": 3, "bank1": {"offset": 768, "size": 8, "words": [1, 2]}},
        {"fingerprint": "00000000000000aa", "pool_offset": 256, "size": 896, "stage": "fragment",
         "tier": 1, "bank1": null},
        {"fingerprint": "0000000000000007", "pool_offset": 1152, "size": 128, "stage": "vertex",
         "tier": 4},
        {"fingerprint": "0000000000000009", "pool_offset": 64, "size": 128, "stage": "vertex",
         "tier": 2},
        {"fingerprint": "0000000000000007", "pool_offset": 1152, "size": 128, "stage": "mesh",
         "tier": 2}
      ]})";
    std::vector<ShaderPoolIndex::ManifestRow> rows;
    REQUIRE(ShaderPoolIndex::ParseManifest(json, rows));
    // Tier 4 is off.
    REQUIRE(rows.size() == 4);
    REQUIRE(rows[0].fingerprint == 0xaa);
    REQUIRE(rows[0].stage == 4);
    REQUIRE(rows[0].bank1_offset == 768);
    REQUIRE(rows[0].bank1_words == std::vector<u32>{1, 2});
    REQUIRE(rows[3].stage == 8);

    std::vector<ShaderPoolIndex::Container> containers(1);
    containers[0].programs.push_back({0x100, 0x380, 4, 0xaa, 768, 8});
    containers[0].programs.push_back({0x480, 0x80, 0, 7, ~0U, 0});
    auto index = ShaderPoolIndex::ForTesting(std::move(containers));
    // Rows 0 and 1 match program 0 (the lowest tier wins, the bank-1 words are kept); row 2
    // names no indexed program; row 3 names program 1's key with another stage.
    REQUIRE(index->ApplyManifest(rows) == 2);
    const auto& programs = index->Containers()[0].programs;
    REQUIRE(programs[0].tier == 1);
    REQUIRE(programs[0].manifest_bank1 == 0);
    REQUIRE(programs[1].tier == ShaderPoolIndex::NoTier);

    REQUIRE_FALSE(ShaderPoolIndex::ParseManifest("{\"programs\": [{\"fingerprint\": \"zz\"}]}",
                                                 rows));
    REQUIRE_FALSE(ShaderPoolIndex::ParseManifest("not json", rows));
}

TEST_CASE("AOT scene manifests replace the previous scene's tiers and keep the global ones",
          "[video_core][translation_cache]") {
    using VideoCommon::ShaderPoolIndex;
    using Row = ShaderPoolIndex::ManifestRow;
    std::vector<ShaderPoolIndex::Container> containers(1);
    containers[0].programs.push_back({0x000, 0x100, 0, 0x10, ~0U, 0});
    containers[0].programs.push_back({0x100, 0x100, 4, 0x11, 0xc0, 8});
    containers[0].programs.push_back({0x200, 0x100, 0, 0x12, ~0U, 0});
    auto index = ShaderPoolIndex::ForTesting(std::move(containers));
    const auto& programs = index->Containers()[0].programs;
    // The global manifest: program 0 at tier 2.
    const std::vector<Row> global{Row{0x10, 0x000, 0x100, 0, 2}};
    REQUIRE(index->ApplyManifest(global) == 1);
    index->KeepTiersAsGlobal();
    // Scene A: programs 0 (tier 1, lower than the global tier) and 1 (with bank-1 words).
    const std::vector<Row> scene_a{Row{0x10, 0x000, 0x100, 0, 1},
                                   Row{0x11, 0x100, 0x100, 4, 3, 0xc0, {5, 6}}};
    REQUIRE(index->SelectSceneRows(scene_a) == 2);
    REQUIRE(programs[0].tier == 1);
    REQUIRE(programs[1].tier == 3);
    REQUIRE(programs[1].manifest_bank1 == 0);
    REQUIRE(programs[2].tier == ShaderPoolIndex::NoTier);
    // Scene B names program 2 only: A's tiers go, the global tier comes back.
    const std::vector<Row> scene_b{Row{0x12, 0x200, 0x100, 0, 1}, Row{0x99, 0x300, 0x80, 0, 1}};
    REQUIRE(index->SelectSceneRows(scene_b) == 1);
    REQUIRE(programs[0].tier == 2);
    REQUIRE(programs[1].tier == ShaderPoolIndex::NoTier);
    REQUIRE(programs[1].manifest_bank1 == ~0U);
    REQUIRE(programs[2].tier == 1);
    // Back to A: the same tiers and a fresh bank-1 slot.
    REQUIRE(index->SelectSceneRows(scene_a) == 2);
    REQUIRE(programs[1].tier == 3);
    REQUIRE(programs[1].manifest_bank1 == 0);
    REQUIRE(programs[2].tier == ShaderPoolIndex::NoTier);
}

TEST_CASE("AOT scene hints: placement reads count before the first entry only",
          "[video_core][translation_cache]") {
    namespace Scene = Common::ShaderAotScene;
    const u64 session = Scene::BeginSession();
    u64 generation{};
    const u64 start = Scene::Generation();
    Scene::Post("TestTitle_Child", false, session); // boot: the save's scene behind the title
    REQUIRE(Scene::Current(generation) == "TestTitle_Child");
    REQUIRE(generation == start + 1);
    Scene::Post("TestTitle_Child", true, session); // entering it changes nothing
    REQUIRE(Scene::Generation() == start + 1);
    const u64 prefetches = Scene::Prefetches();
    Scene::Post("TestField_Child", false, session); // a neighbour prefetched after the entry
    REQUIRE(Scene::Current(generation) == "TestTitle_Child");
    REQUIRE(Scene::Prefetches() == prefetches + 1);
    Scene::Post("TestField_Child", true, session);
    REQUIRE(Scene::Current(generation) == "TestField_Child");
    REQUIRE(generation == start + 2);
}

TEST_CASE("AOT scene hints belong to one emulation session",
          "[video_core][translation_cache]") {
    namespace Scene = Common::ShaderAotScene;
    u64 generation{};
    const u64 first = Scene::BeginSession();
    Scene::SetHookedFiles(240, first);
    Scene::Post("TestA_Child", false, first);
    Scene::Post("TestA_Child", true, first);
    REQUIRE(Scene::HookedFiles() == 240);
    // The next game: nothing of the first session is left, the zero-hook case included.
    const u64 before = Scene::Generation();
    const u64 second = Scene::BeginSession();
    REQUIRE(second != first);
    REQUIRE(Scene::Generation() != before);
    REQUIRE(Scene::HookedFiles() == 0);
    REQUIRE(Scene::Current(generation).empty());
    // A wrapper of the first session (its RomFS outlived the boundary) posts nothing.
    Scene::Post("TestB_Child", true, first);
    Scene::SetHookedFiles(240, first);
    REQUIRE(Scene::Current(generation).empty());
    REQUIRE(Scene::HookedFiles() == 0);
    // Boot placement reads name the scene again, since nothing has been entered yet.
    Scene::Post("TestB_Child", false, second);
    REQUIRE(Scene::Current(generation) == "TestB_Child");
}

TEST_CASE("AOT scene names come from the placement and parameter files only",
          "[video_core][translation_cache]") {
    using Common::ShaderAotScene::SceneOfPath;
    const auto placement = SceneOfPath("/Banc/KokiriForest/KokiriForest_Child_Static.bcett.byml.zs");
    REQUIRE(placement.has_value());
    REQUIRE(placement->scene == "KokiriForest_Child");
    REQUIRE_FALSE(placement->entered);
    REQUIRE(SceneOfPath("Banc/HyruleField/HyruleField_Adult_Dynamic.bcett.byml.zs")->scene ==
            "HyruleField_Adult");
    const auto param =
        SceneOfPath("/Banc/GameBancParam/KokiriForest_Child.game__banc__GameBancParam.bgyml");
    REQUIRE(param.has_value());
    REQUIRE(param->scene == "KokiriForest_Child");
    REQUIRE(param->entered);
    REQUIRE_FALSE(SceneOfPath("/Banc/GameBancParam/X_Static.bcett.byml.zs").has_value());
    REQUIRE_FALSE(SceneOfPath("/Banc/BancBasedResource/StartPos.byml.zs").has_value());
    REQUIRE_FALSE(SceneOfPath("/Banc/A/B/C_Static.bcett.byml.zs").has_value());
    REQUIRE_FALSE(SceneOfPath("/Banc/KokiriForest/_Static.bcett.byml.zs").has_value());
    REQUIRE_FALSE(SceneOfPath("/Pack/Banc/X/Y_Static.bcett.byml.zs").has_value());
}

namespace {
// What the texture pass does for one handle: Begin, the two word reads, End.
u32 ReadHandle(Shader::Environment& env, const Shader::ConstBufferAddr& cbuf) {
    env.BeginTextureHandle();
    const u32 sec_index = cbuf.has_secondary ? cbuf.secondary_index : cbuf.index;
    const u32 sec_offset = cbuf.has_secondary ? cbuf.secondary_offset : cbuf.offset;
    const u32 handle = (env.ReadCbufValue(cbuf.index, cbuf.offset) << cbuf.shift_left) |
                       (env.ReadCbufValue(sec_index, sec_offset) << cbuf.secondary_shift_left);
    env.EndTextureHandle(cbuf, handle);
    return handle;
}

Shader::ConstBufferAddr HandleAt(u32 index, u32 offset) {
    Shader::ConstBufferAddr cbuf{};
    cbuf.index = index;
    cbuf.offset = offset;
    return cbuf;
}

size_t CountKind(const std::vector<Query>& queries, QueryKind kind) {
    return static_cast<size_t>(std::ranges::count(queries, kind, &Query::kind));
}
} // Anonymous namespace

TEST_CASE("Class-level replay compares texture classes instead of handle words",
          "[video_core][translation_cache]") {
    FakeEnvironment env;
    const u64 word = (u64{2} << 32) | 0x20;
    env.cbufs[word] = 5; // a 2D texture
    std::vector<Query> queries;
    {
        RecordingEnvironment recording{env, true};
        const u32 handle = ReadHandle(recording, HandleAt(2, 0x20));
        (void)recording.ReadTextureType(handle);
        queries = recording.TakeQueries();
    }
    // Handle-only word: no exact query, one TextureHandle query.
    REQUIRE(CountKind(queries, QueryKind::CbufValue) == 0);
    REQUIRE(CountKind(queries, QueryKind::TextureHandle) == 1);
    // A different handle of the same class matches, and the live answers carry the new words.
    env.cbufs[word] = 6;
    LiveAnswers live;
    REQUIRE(ReplayQueriesUntilMismatch(queries, env, &live) == queries.size());
    REQUIRE(std::ranges::any_of(live, [](const Query& q) {
        return q.kind == QueryKind::CbufValue && q.answer == 6;
    }));
    REQUIRE(std::ranges::any_of(live, [](const Query& q) {
        return q.kind == QueryKind::TextureType && q.a == 6;
    }));
    // A handle of another class does not.
    env.cbufs[word] = 7; // ColorArray2D in FakeEnvironment
    REQUIRE(ReplayQueriesUntilMismatch(queries, env, &live) < queries.size());
    // Without live answers (the setting off), such an entry never matches.
    env.cbufs[word] = 5;
    REQUIRE_FALSE(ReplayQueries(queries, env));
}

TEST_CASE("Class-level replay keeps exact words exact", "[video_core][translation_cache]") {
    FakeEnvironment env;
    env.cbufs[(u64{1} << 32) | 0x10] = 0x40;
    env.cbufs[(u64{2} << 32) | 0x20] = 5;
    std::vector<Query> queries;
    {
        RecordingEnvironment recording{env, true};
        // Exact-only word (a folded driver constant).
        (void)recording.ReadCbufValue(1, 0x10);
        // Shared word: read for a handle, then consumed by value from the pass's word cache.
        const u32 handle = ReadHandle(recording, HandleAt(2, 0x20));
        recording.NoteExactCbufWord(2, 0x20);
        (void)recording.ReadTextureType(handle);
        queries = recording.TakeQueries();
    }
    REQUIRE(CountKind(queries, QueryKind::CbufValue) == 3); // the exact word, both handle reads
    LiveAnswers live;
    REQUIRE(ReplayQueriesUntilMismatch(queries, env, &live) == queries.size());
    env.cbufs[(u64{1} << 32) | 0x10] = 0x44;
    REQUIRE(ReplayQueriesUntilMismatch(queries, env, &live) < queries.size());
    env.cbufs[(u64{1} << 32) | 0x10] = 0x40;
    env.cbufs[(u64{2} << 32) | 0x20] = 6; // same class, but the word is also used by value
    REQUIRE(ReplayQueriesUntilMismatch(queries, env, &live) < queries.size());
}

TEST_CASE("Class-level replay covers two-word handles and rejects one-to-many mappings",
          "[video_core][translation_cache]") {
    FakeEnvironment env;
    Shader::ConstBufferAddr two_word = HandleAt(2, 0x20);
    two_word.has_secondary = true;
    two_word.secondary_index = 3;
    two_word.secondary_offset = 0x40;
    two_word.secondary_shift_left = 20;
    env.cbufs[(u64{2} << 32) | 0x20] = 5;
    env.cbufs[(u64{3} << 32) | 0x40] = 1;
    env.cbufs[(u64{2} << 32) | 0x30] = 5;
    std::vector<Query> queries;
    {
        RecordingEnvironment recording{env, true};
        const u32 first = ReadHandle(recording, two_word);
        (void)recording.ReadTextureType(first);
        // A second handle recorded with the same value from other words.
        Shader::ConstBufferAddr other = HandleAt(2, 0x30);
        other.has_secondary = true;
        other.secondary_index = 3;
        other.secondary_offset = 0x40;
        other.secondary_shift_left = 20;
        const u32 second = ReadHandle(recording, other);
        REQUIRE(second == first);
        (void)recording.ReadTextureType(second);
        queries = recording.TakeQueries();
    }
    REQUIRE(CountKind(queries, QueryKind::CbufValue) == 0);
    REQUIRE(CountKind(queries, QueryKind::TextureHandle) == 2);
    LiveAnswers live;
    REQUIRE(ReplayQueriesUntilMismatch(queries, env, &live) == queries.size());
    // The secondary word is covered: changing it changes the live handle (7 << 20 here is
    // still a 2D texture, so it matches with a new handle).
    env.cbufs[(u64{3} << 32) | 0x40] = 2;
    REQUIRE(ReplayQueriesUntilMismatch(queries, env, &live) == queries.size());
    // The two recorded-equal handles now differ live: one recorded handle, two live ones.
    env.cbufs[(u64{2} << 32) | 0x30] = 6;
    REQUIRE(ReplayQueriesUntilMismatch(queries, env, &live) < queries.size());
}

namespace {
// Sets an environment variable for one scope and restores it afterwards.
class ScopedEnvironmentVariable {
public:
    ScopedEnvironmentVariable(const char* name_, const char* value) : name{name_} {
        if (const char* old = std::getenv(name)) {
            previous = old;
        }
        Set(value);
    }
    ~ScopedEnvironmentVariable() {
        Set(previous ? previous->c_str() : nullptr);
    }

private:
    void Set(const char* value) {
#ifdef _WIN32
        _putenv_s(name, value ? value : "");
#else
        if (value) {
            setenv(name, value, 1);
        } else {
            unsetenv(name);
        }
#endif
    }

    const char* name;
    std::optional<std::string> previous;
};
} // Anonymous namespace

TEST_CASE("Translation cache identifies the translator by its source",
          "[video_core][translation_cache]") {
    // The generated header holds a SHA-256 in lowercase hexadecimal.
    const auto is_hex = [](std::string_view text) {
        return std::all_of(text.begin(), text.end(),
                           [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
    };
    const std::string_view hash = Shader::TranslatorSourceHash();
    REQUIRE(hash.size() == 64);
    REQUIRE(is_hex(hash));
    // src/shader_recompiler alone holds more than two hundred files.
    REQUIRE(Shader::TranslatorSourceFileCount() > 200);
    // Nothing about the running process: the same value on every call, built from the hash.
    REQUIRE(Shader::TranslatorIdentity() == Shader::TranslatorIdentity());
    REQUIRE(Shader::TranslatorIdentity().find(hash) != std::string::npos);

    const Shader::Profile profile{};
    const Shader::HostTranslateInfo host_info{};
    Digest source_digest{};
    std::string source_tag;
    {
        ScopedEnvironmentVariable key{"EDEN_NX2_TRANSLATION_CACHE_KEY", nullptr};
        const CacheIdentity identity = CurrentCacheIdentity();
        REQUIRE(identity.kind == "source");
        REQUIRE(identity.value == Shader::TranslatorIdentity());
        REQUIRE(identity.file_tag.size() == 16);
        REQUIRE(is_hex(identity.file_tag));
        REQUIRE(identity.description.find(hash.substr(0, 16)) != std::string::npos);
        source_tag = identity.file_tag;
        source_digest = MakeConfigDigest(profile, host_info, 0);
        REQUIRE(MakeConfigDigest(profile, host_info, 0) == source_digest);
        REQUIRE_FALSE(MakeConfigDigest(profile, host_info, 1) == source_digest);
    }
    {
        ScopedEnvironmentVariable key{"EDEN_NX2_TRANSLATION_CACHE_KEY", "source"};
        REQUIRE(CurrentCacheIdentity().file_tag == source_tag);
        REQUIRE(MakeConfigDigest(profile, host_info, 0) == source_digest);
    }
    {
        // An unknown value keeps the source identity and says so in the log line.
        ScopedEnvironmentVariable key{"EDEN_NX2_TRANSLATION_CACHE_KEY", "bogus"};
        const CacheIdentity identity = CurrentCacheIdentity();
        REQUIRE(identity.kind == "source");
        REQUIRE(identity.file_tag == source_tag);
        REQUIRE(identity.description.find("not recognised") != std::string::npos);
        REQUIRE(MakeConfigDigest(profile, host_info, 0) == source_digest);
    }
    {
        // The escape hatch restores the executable identity and the old file name, and never
        // shares a file with the source identity.
        ScopedEnvironmentVariable key{"EDEN_NX2_TRANSLATION_CACHE_KEY", "exe"};
        const CacheIdentity identity = CurrentCacheIdentity();
        REQUIRE(identity.kind == "exe");
        REQUIRE(identity.file_tag.empty());
#if defined(__linux__) || defined(_WIN32)
        REQUIRE_FALSE(identity.value.empty());
        REQUIRE(identity.value.find(hash) == std::string::npos);
        REQUIRE_FALSE(MakeConfigDigest(profile, host_info, 0) == source_digest);
#endif
    }
}

namespace {
// A recipe distinguished by its uniform-buffer binding base (made-up values).
Shader::RuntimeInfo RecipeInfo(u32 variant) {
    Shader::RuntimeInfo info{};
    info.generic_input_types[0] = Shader::AttributeType::Float;
    info.previous_stage_stores.mask.set(variant % 512);
    info.y_negate = (variant & 1) != 0;
    info.alpha_test_func = Shader::CompareFunction::Greater;
    info.xfb_count = 1;
    info.xfb_varyings[0].components = 4;
    return info;
}
Shader::Backend::Bindings RecipeBindings(u32 variant) {
    Shader::Backend::Bindings bindings{};
    bindings.uniform_buffer = variant;
    return bindings;
}
} // Anonymous namespace

TEST_CASE("AOT recipe histogram picks the class's most common recipe and round-trips",
          "[video_core][translation_cache]") {
    using VideoCommon::RecipeHistogram;
    const VideoCommon::TranslationCache::Digest config{1, 2};
    std::bitset<512> loads_a, loads_b;
    loads_a.set(40);
    loads_b.set(41);
    RecipeHistogram histogram;
    REQUIRE(histogram.Record(Shader::Stage::Fragment, loads_a, RecipeInfo(1), RecipeBindings(1)));
    REQUIRE_FALSE(
        histogram.Record(Shader::Stage::Fragment, loads_a, RecipeInfo(2), RecipeBindings(2)));
    histogram.Record(Shader::Stage::Fragment, loads_a, RecipeInfo(2), RecipeBindings(2));
    histogram.Record(Shader::Stage::Fragment, loads_a, RecipeInfo(2), RecipeBindings(2));
    bool class_match = false;
    auto best = histogram.Best(Shader::Stage::Fragment, loads_a, class_match);
    REQUIRE(best.has_value());
    REQUIRE(class_match);
    REQUIRE(best->binding_in.uniform_buffer == 2);
    // Another class of the stage falls back to the stage's recipes.
    best = histogram.Best(Shader::Stage::Fragment, loads_b, class_match);
    REQUIRE(best.has_value());
    REQUIRE_FALSE(class_match);

    const std::vector<u8> data = histogram.Serialize(config);
    RecipeHistogram loaded;
    std::string why;
    REQUIRE(loaded.Load(data, config, why));
    REQUIRE(loaded.Classes() == histogram.Classes());
    REQUIRE(loaded.Recipes() == histogram.Recipes());
    best = loaded.Best(Shader::Stage::Fragment, loads_a, class_match);
    REQUIRE(best.has_value());
    REQUIRE_FALSE(best->seen);
    REQUIRE(best->runtime_info.alpha_test_func == Shader::CompareFunction::Greater);
    REQUIRE(best->runtime_info.xfb_count == 1);
    REQUIRE(best->binding_in.uniform_buffer == 2);
    REQUIRE(best->count == 2); // 3 halved, rounded up

    REQUIRE_FALSE(loaded.Load(data, VideoCommon::TranslationCache::Digest{9, 9}, why));
    REQUIRE(loaded.Classes() == 0);
    std::vector<u8> truncated(data.begin(), data.end() - 3);
    REQUIRE_FALSE(loaded.Load(truncated, config, why));
    REQUIRE(loaded.Classes() == 0);
}

TEST_CASE("AOT recipe file: atomic replacement through a unique temporary file",
          "[video_core][translation_cache]") {
    const auto dir = TempPath("eden-aot-recipe-write");
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto path = dir / "recipes.bin";
    std::string why;
    const std::vector<u8> first{1, 2, 3}, second{4, 5, 6, 7};
    REQUIRE(VideoCommon::WriteFileAtomically(path, first, why));
    // A stale temporary of another writer under the old fixed name is neither used nor touched.
    {
        std::ofstream stale{dir / "recipes.bin.tmp", std::ios::binary};
        stale << "other writer";
    }
    REQUIRE(VideoCommon::WriteFileAtomically(path, second, why));
    std::ifstream in{path, std::ios::binary};
    const std::vector<u8> read{std::istreambuf_iterator<char>{in}, {}};
    REQUIRE(read == second);
    size_t files = 0;
    for ([[maybe_unused]] const auto& item : std::filesystem::directory_iterator(dir)) {
        ++files;
    }
    REQUIRE(files == 2); // the file and the stale temporary, no temporary of ours left
    // A missing directory fails without leaving anything behind.
    REQUIRE_FALSE(VideoCommon::WriteFileAtomically(dir / "missing" / "recipes.bin", first, why));
    REQUIRE_FALSE(why.empty());
    std::filesystem::remove_all(dir);
}

TEST_CASE("AOT recipe file: oversized, repeated classes and repeated recipes are refused",
          "[video_core][translation_cache]") {
    using VideoCommon::RecipeHistogram;
    const VideoCommon::TranslationCache::Digest config{3, 4};
    std::bitset<512> loads;
    loads.set(7);
    RecipeHistogram histogram;
    histogram.Record(Shader::Stage::Fragment, loads, RecipeInfo(1), RecipeBindings(1));
    std::vector<u8> data = histogram.Serialize(config);
    std::string why;
    RecipeHistogram loaded;
    REQUIRE(loaded.Load(data, config, why));
    REQUIRE(loaded.Classes() == 2); // the class and its stage

    // A size above the format's maximum is refused before anything is read.
    std::istringstream empty{std::string{}};
    REQUIRE_FALSE(loaded.Load(empty, RecipeHistogram::MaxFileBytes() + 1, config, why));
    REQUIRE(why == "larger than the format allows");
    REQUIRE(loaded.Classes() == 0);

    // The class count is at offset 28 (magic, version, configuration digest): claim both
    // classes twice by appending the class records again.
    std::vector<u8> doubled = data;
    doubled.insert(doubled.end(), data.begin() + 32, data.end());
    const u32 four = 4;
    std::memcpy(doubled.data() + 28, &four, sizeof(four));
    REQUIRE_FALSE(loaded.Load(doubled, config, why));
    REQUIRE(why == "a class repeated");

    // One class whose two recipes are the same. The file holds two classes (the program's and
    // its stage's), one recipe each, of the same size; take the first and repeat its recipe.
    const size_t header = 32; // magic, version, configuration digest, class count
    const size_t stage_size = sizeof(Shader::Stage);
    const size_t heads = 2 * (stage_size + 1 + 4) + 64;
    const size_t recipe_size = (data.size() - header - heads) / 2;
    const bool first_has_loads = data[header + stage_size] != 0;
    const size_t count_at = header + stage_size + 1 + (first_has_loads ? 64 : 0);
    const size_t recipe_begin = count_at + 4;
    std::vector<u8> repeated(data.begin(),
                             data.begin() + static_cast<std::ptrdiff_t>(recipe_begin + recipe_size));
    repeated.insert(repeated.end(), data.begin() + static_cast<std::ptrdiff_t>(recipe_begin),
                    data.begin() + static_cast<std::ptrdiff_t>(recipe_begin + recipe_size));
    const u32 two = 2, one_class = 1;
    std::memcpy(repeated.data() + count_at, &two, sizeof(two));
    std::memcpy(repeated.data() + 28, &one_class, sizeof(one_class));
    REQUIRE_FALSE(loaded.Load(repeated, config, why));
    REQUIRE(why == "a recipe repeated in its class");
}

TEST_CASE("AOT recipe histogram revisions tell a pending save",
          "[video_core][translation_cache]") {
    using VideoCommon::RecipeHistogram;
    std::bitset<512> loads;
    loads.set(9);
    RecipeHistogram histogram;
    const u64 empty = histogram.Revision();
    histogram.Record(Shader::Stage::VertexB, loads, RecipeInfo(1), RecipeBindings(1));
    const u64 snapshot = histogram.Revision();
    REQUIRE(snapshot != empty);
    // A recording made while the snapshot is written leaves a newer revision pending.
    histogram.Record(Shader::Stage::VertexB, loads, RecipeInfo(1), RecipeBindings(1));
    REQUIRE(histogram.Revision() > snapshot);
    // A failed write (here: into a missing directory) reports it, so the caller keeps the
    // revision pending.
    std::string why;
    REQUIRE_FALSE(VideoCommon::WriteFileAtomically(
        TempPath("eden-aot-missing-dir") / "x" / "recipes.bin",
        histogram.Serialize(VideoCommon::TranslationCache::Digest{}), why));
}

TEST_CASE("AOT recipe histogram: a full persisted class learns a new dominant recipe",
          "[video_core][translation_cache]") {
    using VideoCommon::RecipeHistogram;
    const VideoCommon::TranslationCache::Digest config{5, 6};
    std::bitset<512> loads;
    loads.set(11);
    RecipeHistogram earlier;
    for (u32 variant = 0; variant < RecipeHistogram::MaxPerClass; ++variant) {
        for (int i = 0; i < 4; ++i) {
            earlier.Record(Shader::Stage::Fragment, loads, RecipeInfo(variant),
                           RecipeBindings(variant));
        }
    }
    RecipeHistogram histogram;
    std::string why;
    REQUIRE(histogram.Load(earlier.Serialize(config), config, why));
    REQUIRE(histogram.Recipes() == 2 * RecipeHistogram::MaxPerClass); // class and stage, full
    // A recipe the earlier sessions never saw, now the common one.
    const u32 fresh = 100;
    bool class_match = false;
    for (int i = 0; i < 3; ++i) {
        histogram.Record(Shader::Stage::Fragment, loads, RecipeInfo(fresh), RecipeBindings(fresh));
    }
    REQUIRE(histogram.Recipes() == 2 * RecipeHistogram::MaxPerClass);
    auto best = histogram.Best(Shader::Stage::Fragment, loads, class_match);
    REQUIRE(best.has_value());
    REQUIRE(class_match);
    REQUIRE(best->binding_in.uniform_buffer == fresh); // 3 against the halved 2
    REQUIRE(best->seen);

    // In-session: a full class of recipes seen twice keeps them against a one-off.
    RecipeHistogram session;
    for (u32 variant = 0; variant < RecipeHistogram::MaxPerClass; ++variant) {
        session.Record(Shader::Stage::Fragment, loads, RecipeInfo(variant),
                       RecipeBindings(variant));
        session.Record(Shader::Stage::Fragment, loads, RecipeInfo(variant),
                       RecipeBindings(variant));
    }
    session.Record(Shader::Stage::Fragment, loads, RecipeInfo(fresh), RecipeBindings(fresh));
    best = session.Best(Shader::Stage::Fragment, loads, class_match);
    REQUIRE(best->binding_in.uniform_buffer != fresh);
}

TEST_CASE("AOT candidates without room go back to the queue in order",
          "[video_core][translation_cache]") {
    using VideoCommon::ShaderPoolIndex;
    auto index = ShaderPoolIndex::ForTesting({});
    for (u32 i = 0; i < 4; ++i) {
        index->QueueForTesting({0x1000 + i * 0x100, 0, 1, ~0U, 0, ~0U});
    }
    auto taken = index->TakeCandidates(4);
    REQUIRE(taken.size() == 4);
    REQUIRE(index->QueuedCandidates() == 0);
    // One slot was free: the first is seeded, the other three wait for room.
    index->Acknowledge(taken[0]);
    index->ReturnCandidates(std::span{taken}.subspan(1));
    REQUIRE(index->QueuedCandidates() == 3);
    REQUIRE(index->HasManifestCandidates());
    const auto again = index->TakeCandidates(4);
    REQUIRE(again.size() == 3);
    REQUIRE(again[0].address == taken[1].address);
    REQUIRE(again[2].address == taken[3].address);
}

TEST_CASE("AOT recipe histogram: a ninth recipe overtakes eight residents seen more than once",
          "[video_core][translation_cache]") {
    using VideoCommon::RecipeHistogram;
    std::bitset<512> loads;
    loads.set(13);
    RecipeHistogram histogram;
    for (u32 variant = 0; variant < RecipeHistogram::MaxPerClass; ++variant) {
        for (int i = 0; i < 2; ++i) {
            histogram.Record(Shader::Stage::Fragment, loads, RecipeInfo(variant),
                             RecipeBindings(variant));
        }
    }
    const u32 fresh = 200;
    bool class_match = false;
    // Twice: tallied aside (2 is not above the residents' 2), still not chosen.
    for (int i = 0; i < 2; ++i) {
        histogram.Record(Shader::Stage::Fragment, loads, RecipeInfo(fresh), RecipeBindings(fresh));
    }
    REQUIRE(histogram.Best(Shader::Stage::Fragment, loads, class_match)->binding_in.uniform_buffer !=
            fresh);
    // The third observation promotes it over the weakest resident; it is now the best.
    histogram.Record(Shader::Stage::Fragment, loads, RecipeInfo(fresh), RecipeBindings(fresh));
    REQUIRE(histogram.Recipes() == 2 * RecipeHistogram::MaxPerClass);
    const auto best = histogram.Best(Shader::Stage::Fragment, loads, class_match);
    REQUIRE(class_match);
    REQUIRE(best->binding_in.uniform_buffer == fresh);
    REQUIRE(best->count == 3);
}

TEST_CASE("AOT candidates at unmapped addresses are retried a bounded number of times",
          "[video_core][translation_cache]") {
    using VideoCommon::ShaderPoolIndex;
    auto index = ShaderPoolIndex::ForTesting({});
    const ShaderPoolIndex::Candidate candidate{0x2000, 4, 1, ~0U, 0, ~0U};
    index->QueueForTesting(candidate);
    auto now = std::chrono::steady_clock::now();
    for (u32 attempt = 1; attempt <= ShaderPoolIndex::MaxRetries; ++attempt) {
        auto taken = index->TakeCandidates(4);
        REQUIRE(taken.size() == 1);
        index->Defer(taken[0]); // unmapped when taken
        now = std::chrono::steady_clock::now();
        REQUIRE(index->QueuedCandidates() == 0);
        REQUIRE(index->DeferredCandidates() == 1);
        index->RequeueDeferred(now); // not yet: it waits for its retry time
        REQUIRE(index->QueuedCandidates() == 0);
        index->RequeueDeferred(now + ShaderPoolIndex::RetryInterval * attempt);
        REQUIRE(index->QueuedCandidates() == 1);
        REQUIRE(index->DeferredCandidates() == 0);
    }
    // One retry too many: dropped (a scene change can still offer it).
    auto taken = index->TakeCandidates(4);
    index->Defer(taken[0]);
    REQUIRE(index->DeferredCandidates() == 0);
    index->RequeueDeferred(now + std::chrono::hours{1});
    REQUIRE(index->QueuedCandidates() == 0);
}
