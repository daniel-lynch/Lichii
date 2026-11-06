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
#include <string>
#include <string_view>
#include <catch2/catch_test_macros.hpp>
#include "common/logging.h"
#include "common/fs/path_util.h"
#include "video_core/shader_pool_index.h"
#include "shader_recompiler/translator_identity.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "video_core/shader_translation_cache.h"
#include "video_core/shader_archive_environment.h"

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

TEST_CASE("Translation cache separates fragment shuffle workaround profiles",
          "[video_core][translation_cache][fragment_shuffle]") {
    Shader::Profile profile{};
    const Shader::HostTranslateInfo host_info{};
    const auto native = MakeConfigDigest(profile, host_info, 0);
    profile.has_broken_spirv_fragment_shuffle = true;
    REQUIRE(MakeConfigDigest(profile, host_info, 0) != native);
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
        REQUIRE(InfoEqual(found->info, variant.info));
        REQUIRE(store.HasVariant(variant_key));
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

TEST_CASE("Class-level replay compares texture classes instead of handle words with live answers",
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
    // Validation also works without collecting a worker replay snapshot.
    env.cbufs[word] = 5;
    REQUIRE(ReplayQueries(queries, env));
}

TEST_CASE("Class-level replay keeps exact words exact with live answers", "[video_core][translation_cache]") {
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
    REQUIRE(CountKind(queries, QueryKind::CbufValue) == 4); // exact word, handle reads and explicit exact notification
    LiveAnswers live;
    REQUIRE(ReplayQueriesUntilMismatch(queries, env, &live) == queries.size());
    env.cbufs[(u64{1} << 32) | 0x10] = 0x44;
    REQUIRE(ReplayQueriesUntilMismatch(queries, env, &live) < queries.size());
    env.cbufs[(u64{1} << 32) | 0x10] = 0x40;
    env.cbufs[(u64{2} << 32) | 0x20] = 6; // same class, but the word is also used by value
    REQUIRE(ReplayQueriesUntilMismatch(queries, env, &live) < queries.size());
}

TEST_CASE("Class-level replay covers two-word handles and rejects one-to-many mappings with live answers",
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
    // A different handle of the same class matches.
    env.cbufs[word] = 6;
    REQUIRE(ReplayQueries(queries, env));
    // A handle of another class does not.
    env.cbufs[word] = 7; // ColorArray2D in FakeEnvironment
    REQUIRE(!ReplayQueries(queries, env));
    // Restoring the original handle also matches.
    env.cbufs[word] = 5;
    REQUIRE(ReplayQueries(queries, env));
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
    REQUIRE(CountKind(queries, QueryKind::CbufValue) == 4); // exact word, handle reads and explicit exact notification
    REQUIRE(ReplayQueries(queries, env));
    env.cbufs[(u64{1} << 32) | 0x10] = 0x44;
    REQUIRE(!ReplayQueries(queries, env));
    env.cbufs[(u64{1} << 32) | 0x10] = 0x40;
    env.cbufs[(u64{2} << 32) | 0x20] = 6; // same class, but the word is also used by value
    REQUIRE(!ReplayQueries(queries, env));
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
    REQUIRE(ReplayQueries(queries, env));
    // The secondary word is covered: changing it changes the live handle (7 << 20 here is
    // still a 2D texture, so it matches with a new handle).
    env.cbufs[(u64{3} << 32) | 0x40] = 2;
    REQUIRE(ReplayQueries(queries, env));
    // The two recorded-equal handles now differ live: one recorded handle, two live ones.
    env.cbufs[(u64{2} << 32) | 0x30] = 6;
    REQUIRE(!ReplayQueries(queries, env));
}

TEST_CASE("Class replay rejects unrepresentable recipes and malformed shifts",
          "[video_core][translation_cache]") {
    FakeEnvironment env;
    RecordingEnvironment recording{env, true};
    auto address = HandleAt(2, 0x20);
    address.has_secondary = true;
    address.secondary_offset = 0x10000;
    REQUIRE_THROWS(recording.EndTextureHandle(address, 1));
    Query invalid{QueryKind::TextureHandle, 2U | (32U << 8), 0x20, 1, 0};
    REQUIRE_FALSE(ReplayQueries(std::span{&invalid, 1}, env));
    REQUIRE(env.reads == 0);
}

TEST_CASE("Offline metadata can be reused after replay and is dropped on disagreement",
          "[translation_cache][video_core]") {
    FakeEnvironment env;
    Store store;
    store.Open({}, {});
    Shader::IR::Program live;
    live.stage = Shader::Stage::VertexB;
    live.info.stores.Set(Shader::IR::Attribute::Generic0X, true);
    auto guessed = ProgramMeta::From(live);
    guessed.info.stores.Set(Shader::IR::Attribute::Generic0X, false);
    const auto entry = store.Insert({91, 1}, {}, guessed, true);
    const auto recipe = DefaultEmissionRecipe(live.stage);
    const auto key = MakeVariantKey(entry->id, DigestRuntimeInfo(recipe.runtime), recipe.bindings);
    store.InsertVariant(*entry, key, {.spirv = {0x07230203, 0x10000, 0, 1, 0}});
    REQUIRE(store.FindEntry({91, 1}, env));
    REQUIRE(entry->CanReuseMetadata());
    auto other = recipe.runtime;
    other.convert_depth_mode = !other.convert_depth_mode;
    REQUIRE_FALSE(store.FindVariant(MakeVariantKey(entry->id, DigestRuntimeInfo(other), recipe.bindings)));
    // A fresh translation that disagrees invalidates both metadata and modules.
    REQUIRE_FALSE(store.ValidateOfflineEntry(*entry, live));
    REQUIRE(live.info.stores[Shader::IR::Attribute::Generic0X]);
    REQUIRE_FALSE(store.FindEntry({91, 1}, env));
    REQUIRE_FALSE(store.FindVariant(key));
    const auto demand = store.Insert({91, 1}, {}, ProgramMeta::From(live));
    REQUIRE(demand->id != entry->id);
    REQUIRE(demand->CanReuseMetadata());
    REQUIRE(store.FindEntry({91, 1}, env)->id == demand->id);
}

TEST_CASE("Offline entry-only records remain reusable after load and truncated append",
          "[translation_cache][video_core]") {
    const auto path = std::filesystem::temp_directory_path() / "lichii_offline_entry_only_test.bin";
    std::filesystem::remove(path);
    Digest config{91, 2};
    FakeEnvironment env;
    {
        Store store;
        store.Open(path, config);
        REQUIRE(store.Persistent());
        const auto offline = store.Insert({1, 2}, {}, {}, true);
        REQUIRE(store.FindEntry({1, 2}, env));
        REQUIRE(offline);
        REQUIRE(store.Insert({3, 4}, {}, {}));
    }
    {
        std::ofstream tail(path, std::ios::binary | std::ios::app);
        tail.write("STCR", 4);
    }
    {
        Store store;
        store.Open(path, config);
        REQUIRE(store.FindEntry({1, 2}, env));
        REQUIRE(store.FindEntry({3, 4}, env));
        REQUIRE(store.offline_entry_only_rejected.load() == 0);
    }
    std::filesystem::remove(path);
}

TEST_CASE("Offline matching modules and persistent metadata rejection round trip",
          "[translation_cache][video_core]") {
    const auto path = std::filesystem::temp_directory_path() / "lichii_offline_drop_test.bin";
    std::filesystem::remove(path);
    Digest config{91, 3};
    FakeEnvironment env;
    Shader::IR::Program live;
    live.stage = Shader::Stage::Fragment;
    Digest variant_key{};
    {
        Store store;
        store.Open(path, config);
        const auto entry = store.Insert({1, 2}, {}, ProgramMeta::From(live), true);
        auto recipe = DefaultEmissionRecipe(live.stage);
        variant_key = MakeVariantKey(entry->id, DigestRuntimeInfo(recipe.runtime), recipe.bindings);
        store.InsertVariant(*entry, variant_key,
                            {.spirv = {0x07230203, 0x10000, 0, 1, 0}, .fallback_recipe = true}, &recipe);
    }
    {
        Store store;
        store.Open(path, config);
        const auto entry = store.FindEntry({1, 2}, env);
        REQUIRE(entry);
        REQUIRE(entry->offline);
        REQUIRE(store.offline_entry_only_rejected.load() == 0);
        REQUIRE(store.ValidateOfflineEntry(*entry, live));
        const auto variant = store.FindVariant(variant_key);
        REQUIRE(variant);
        REQUIRE(variant->fallback_recipe);
        live.output_vertices = 19; // Scalar metadata matters as well as Shader::Info.
        REQUIRE_FALSE(store.ValidateOfflineEntry(*entry, live));
    }
    {
        Store store;
        store.Open(path, config);
        REQUIRE_FALSE(store.FindEntry({1, 2}, env));
        REQUIRE_FALSE(store.FindVariant(variant_key));
    }
    std::filesystem::remove(path);
}

TEST_CASE("Canonical vertex and fragment keys ignore only unused runtime inputs",
          "[translation_cache][video_core]") {
    for (const auto stage : {Shader::Stage::VertexB, Shader::Stage::Fragment}) {
        Shader::IR::Program program;
        program.stage = stage;
        program.info.loads.Set(Shader::IR::Attribute::Generic0X, true);
        program.info.passthrough.Set(Shader::IR::Attribute::Generic0X + 8, true);
        auto a = DefaultEmissionRecipe(stage).runtime;
        auto b = a;
        b.generic_input_types[1] = Shader::AttributeType::SignedInt;
        b.previous_stage_stores.Set(Shader::IR::Attribute::Generic0X + 4, false);
        b.input_topology = Shader::InputTopology::Triangles;
        const auto digest = [&](const auto& runtime) {
            return DigestRuntimeInfo(CanonicalRuntimeInfo(runtime, program.info, program.stage));
        };
        REQUIRE(DigestRuntimeInfo(a) != DigestRuntimeInfo(b));
        REQUIRE(digest(a) == digest(b));
        SECTION("loaded input type remains significant") {
            b.generic_input_types[0] = Shader::AttributeType::SignedInt;
            REQUIRE(digest(a) != digest(b));
        }
        SECTION("loaded input stores remain significant") {
            b.previous_stage_stores.Set(Shader::IR::Attribute::Generic0X, false);
            REQUIRE(digest(a) != digest(b));
        }
        SECTION("passthrough stores remain significant") {
            b.previous_stage_stores.Set(Shader::IR::Attribute::Generic0X + 8, false);
            REQUIRE(digest(a) != digest(b));
        }
        SECTION("geometry is unchanged") {
            program.stage = Shader::Stage::Geometry;
            REQUIRE(digest(a) != digest(b));
            REQUIRE(digest(b) == DigestRuntimeInfo(b));
        }
        SECTION("legacy varying programs are unchanged") {
            program.info.loads.Set(Shader::IR::Attribute::ColorFrontDiffuseR, true);
            REQUIRE(digest(a) != digest(b));
            REQUIRE(digest(b) == DigestRuntimeInfo(b));
        }
    }
}

TEST_CASE("Offline resume replays guessed handles in their original scope",
          "[video_core][translation_cache]") {
    std::vector<u8> bytes(0x200), control(0x58);
    auto put = [](auto& out, size_t offset, u32 word) {
        std::memcpy(out.data() + offset, &word, sizeof(word));
    };
    put(bytes, 0, 1 << 10);
    put(bytes, 0x80, 0x0000794d);
    put(bytes, 0x88, 0x03800000);
    put(control, 0, 0x98761234);
    put(control, 4, 0xc9);
    put(control, 8, 20);
    put(control, 0x38, 0x90);
    VideoCommon::ArchiveEnvironment env(bytes, control, Shader::Stage::VertexB);
    RecordingEnvironment recording(env, true);
    auto handle = ReadHandle(recording, HandleAt(2, 0x20));
    (void)recording.ReadTextureType(handle);
    const auto queries = recording.TakeQueries();
    VideoCommon::ArchiveEnvironment resumed(bytes, control, Shader::Stage::VertexB);
    RecordingEnvironment replay(resumed, true);
    REQUIRE(ReplayQueries(queries, replay));
    REQUIRE_THROWS(resumed.ReadCbufValue(2, 0x20));
}

TEST_CASE("Read-only translation caches retain usable records without throwing",
          "[video_core][translation_cache]") {
    const auto path = TempPath("lichii_readonly_translation_cache.bin");
    const Digest config{11, 22}, key{33, 44};
    FakeEnvironment env;
    std::filesystem::remove(path);
    Digest variant_key;
    {
        Store store;
        store.Open(path, config);
        const auto entry = store.Insert(key, {}, {.stage = Shader::Stage::Fragment});
        variant_key = MakeVariantKey(entry->id, {}, {});
        REQUIRE(store.InsertVariant(*entry, variant_key, {.spirv = {0x07230203, 1, 2}}));
    }
    SECTION("valid file cannot be opened for append") {}
    SECTION("incomplete tail cannot be truncated") {
        std::ofstream tail(path, std::ios::binary | std::ios::app);
        tail << "broken";
    }
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code ec;
            std::filesystem::permissions(path, std::filesystem::perms::owner_all, ec);
            std::filesystem::remove(path, ec);
            Common::Log::SetRingCapacity(0);
        }
    } cleanup{path};
    std::error_code ec;
    std::filesystem::permissions(path, std::filesystem::perms::owner_read |
        std::filesystem::perms::group_read | std::filesystem::perms::others_read, ec);
    if (ec || std::ofstream(path, std::ios::binary | std::ios::app).is_open())
        SKIP("Read-only file permissions are not enforceable for this platform/user");
    Common::FS::SetEdenPath(Common::FS::EdenPath::LogDir, path.parent_path());
    Common::Log::Initialize();
    Common::Log::SetGlobalFilter(Common::Log::Filter{Common::Log::Level::Info});
    Common::Log::SetRingCapacity(64);
    const auto size = std::filesystem::file_size(path);
    Store store{0};
    REQUIRE_NOTHROW(store.Open(path, config));
    REQUIRE(store.Enabled());
    REQUIRE_FALSE(store.Persistent());
    REQUIRE(store.FindEntry(key, env));
    REQUIRE(store.FindVariant(variant_key));
    const auto new_entry = store.Insert({55, 66}, {}, {});
    REQUIRE(new_entry);
    const auto new_key = MakeVariantKey(new_entry->id, {}, {});
    const Variant new_variant{.binding_out = {.unified = 7, .texture = 9},
                              .info = SampleInfo(), .spirv = {0x07230203, 4, 5, 6}};
    REQUIRE(store.InsertVariant(*new_entry, new_key, new_variant));
    const auto reads_before = store.variant_disk_reads.load();
    const auto found = store.FindVariant(new_key);
    REQUIRE(store.variant_disk_reads == reads_before);
    REQUIRE(store.ResidentBytes() > 0);
    REQUIRE(found);
    REQUIRE(InfoEqual(found->info, new_variant.info));
    REQUIRE(MakeVariantKey({}, {}, found->binding_out) ==
            MakeVariantKey({}, {}, new_variant.binding_out));
    REQUIRE(found->spirv == new_variant.spirv);
    REQUIRE(std::filesystem::file_size(path) == size);
    const auto lines = Common::Log::ReadRing(0);
    REQUIRE(std::ranges::count_if(lines, [&](const auto& line) {
        return line.text.find("persistence disabled for " + path.string()) != std::string::npos &&
               line.text.find(std::make_error_code(std::errc::permission_denied).message()) != std::string::npos;
    }) == 1);
}

TEST_CASE("Recipe catalogs cap exact selection and total import size",
          "[video_core][translation_cache]") {
    const auto path = TempPath("lichii_recipe_limits.bin");
    std::filesystem::remove(path);
    const Digest config{71, 72}, key{73, 74};
    const auto stage = Shader::Stage::Fragment;
    {
        Store source;
        source.Open(path, config);
        for (u32 id = 0; id < 3; ++id) {
            const auto entry = source.Insert(key, {{QueryKind::CbufValue, 0, id, id}}, {.stage = stage});
            for (u32 texture = 1; texture <= (id == 0 ? 6 : 1); ++texture) {
                auto recipe = DefaultEmissionRecipe(stage);
                recipe.bindings.texture = texture;
                REQUIRE(source.InsertVariant(*entry,
                    MakeVariantKey(entry->id, DigestRuntimeInfo(recipe.runtime), recipe.bindings),
                    {.spirv = {0x07230203, 1, 2}}, &recipe));
            }
        }
    }
    const auto catalog = Store::ReadRecipeCatalog(path, config);
    REQUIRE(catalog.exact.at(key).size() == 6);
    REQUIRE(catalog.exact.at(key).front().recipe.bindings.texture == 1);
    REQUIRE(catalog.exact.at(key).front().count == 3);
    REQUIRE(catalog.entries == 18);
    REQUIRE(catalog.truncated_entries == 0);
    const auto selected = catalog.Select(key, stage, {}, 2, RecipeBaseline::Always);
    REQUIRE(selected.size() == 3);
    REQUIRE(selected.front().bindings.texture == 1);
    REQUIRE(selected.back().bindings.texture == 0);
    Common::FS::SetEdenPath(Common::FS::EdenPath::LogDir, path.parent_path());
    Common::Log::Initialize();
    Common::Log::SetGlobalFilter(Common::Log::Filter{Common::Log::Level::Info});
    Common::Log::SetRingCapacity(64);
    const auto capped = Store::ReadRecipeCatalog(path, config, 3);
    REQUIRE(capped.entries == 3);
    REQUIRE(capped.truncated_entries == 15);
    REQUIRE(capped.exact.at(key).size() == 1);
    const auto budgeted = Store::ReadRecipeCatalog(path, config, 200000, 1);
    REQUIRE(budgeted.entries == 0);
    REQUIRE(budgeted.imported_bytes == 0);
    REQUIRE(budgeted.skipped_records == 8);
    const auto lines = Common::Log::ReadRing(0);
    REQUIRE(std::ranges::any_of(lines, [](const auto& line) {
        return line.text.find("3 catalog entries; 15 recipe references truncated by entry limit 3") != std::string::npos;
    }));
    REQUIRE(std::ranges::any_of(lines, [](const auto& line) {
        return line.text.find("8 records skipped by byte budget 1; 0 bytes admitted") != std::string::npos;
    }));
    Common::Log::SetRingCapacity(0);
    std::filesystem::remove(path);
}

TEST_CASE("Recipe import skips malformed fields and continues with later records",
          "[video_core][translation_cache]") {
    const auto path = TempPath("lichii_invalid_recipe.bin");
    std::filesystem::remove(path);
    const Digest config{81, 82}, key{83, 84};
    const auto stage = Shader::Stage::VertexB;
    auto bad = DefaultEmissionRecipe(stage);
    bad.runtime.xfb_count = 33;
    bad.runtime.xfb_varyings[32] = {0, 0, 16, 0, 4};
    SECTION("nine components") { bad.runtime.xfb_varyings[32].components = 9; }
    SECTION("buffer outside hardware slots") { bad.runtime.xfb_varyings[32].buffer = 4; }
    SECTION("stream outside hardware slots") { bad.runtime.xfb_varyings[32].stream = 4; }
    SECTION("offset outside stride") { bad.runtime.xfb_varyings[32].offset = 16; }
    SECTION("oversized stride") { bad.runtime.xfb_varyings[32].stride = UINT32_MAX; }
    SECTION("partial unused slot") { bad.runtime.xfb_varyings[32].components = 0; }
    SECTION("invalid attribute slot") { bad.runtime.xfb_varyings[0] = {0, 0, 4, 0, 1}; }
    SECTION("vector boundary") {
        bad.runtime.xfb_varyings[31] = {0, 0, 16, 0, 2};
    }
    SECTION("input type enum") {
        bad.runtime.generic_input_types[0] = static_cast<Shader::AttributeType>(255);
    }
    SECTION("color type enum") {
        bad.runtime.color_output_types[0] = static_cast<Shader::AttributeType>(255);
    }
    SECTION("topology enum") { bad.runtime.input_topology = static_cast<Shader::InputTopology>(-1); }
    SECTION("tessellation primitive enum") { bad.runtime.tess_primitive = static_cast<Shader::TessPrimitive>(3); }
    SECTION("tessellation spacing enum") { bad.runtime.tess_spacing = static_cast<Shader::TessSpacing>(3); }
    SECTION("compare enum") { bad.runtime.alpha_test_func = static_cast<Shader::CompareFunction>(8); }
    SECTION("mapping source") {
        bad.runtime.previous_stage_legacy_stores_mapping[static_cast<Shader::IR::Attribute>(512)] = Shader::IR::Attribute::Generic0X;
    }
    SECTION("mapping target") {
        bad.runtime.previous_stage_legacy_stores_mapping[Shader::IR::Attribute::ColorFrontDiffuseR] = static_cast<Shader::IR::Attribute>(512);
    }
    SECTION("point size") { bad.runtime.fixed_state_point_size = -1.0f; }
    SECTION("binding overflow") { bad.bindings.texture = UINT32_MAX; }
    SECTION("scaling index") { bad.bindings.texture_scaling_index = 129; }
    {
        Store source;
        source.Open(path, config);
        const auto entry = source.Insert(key, {}, {.stage = stage});
        // The writer supplies the framing; this record's recipe payload is deliberately malformed.
        REQUIRE(source.InsertVariant(*entry, {91, 92}, {.spirv = {0x07230203, 1, 2}}, &bad));
        const auto good = DefaultEmissionRecipe(stage);
        REQUIRE(source.InsertVariant(*entry, {93, 94}, {.spirv = {0x07230203, 1, 2}}, &good));
    }
    Common::FS::SetEdenPath(Common::FS::EdenPath::LogDir, path.parent_path());
    Common::Log::Initialize();
    Common::Log::SetGlobalFilter(Common::Log::Filter{Common::Log::Level::Info});
    Common::Log::SetRingCapacity(64);
    const auto size = std::filesystem::file_size(path);
    const auto catalog = Store::ReadRecipeCatalog(path, config);
    REQUIRE(catalog.invalid_recipes == 1);
    REQUIRE(catalog.exact.at(key).size() == 1);
    const auto lines = Common::Log::ReadRing(0);
    REQUIRE(std::ranges::any_of(lines, [&](const auto& line) {
        return line.text.find("skipped 1 invalid recipes in " + path.string()) != std::string::npos;
    }));
    Common::Log::SetRingCapacity(0);
    Store runtime;
    REQUIRE_NOTHROW(runtime.Open(path, config));
    REQUIRE(runtime.Persistent());
    REQUIRE_FALSE(runtime.FindVariant({91, 92}));
    REQUIRE(runtime.FindVariant({93, 94}));
    REQUIRE(std::filesystem::file_size(path) == size);
    runtime.Close();
    std::filesystem::remove(path);
}

TEST_CASE("SPIR-V emission bounds transform-feedback components to each output vector",
          "[video_core][translation_cache]") {
    Shader::IR::Program program;
    program.stage = Shader::Stage::VertexB;
    program.info.stores.Set(Shader::IR::Attribute::Generic0X);
    Shader::Profile profile{};
    profile.supported_spirv = 0x00010300;
    Shader::RuntimeInfo runtime{};
    runtime.xfb_count = 36;
    runtime.xfb_varyings[32] = {0, 0, 16, 0, 9};
    u32 first_components = 4;
    SECTION("oversized first vector") {}
    SECTION("oversized remaining component") {
        runtime.xfb_varyings[32].components = 3;
        runtime.xfb_varyings[35] = {0, 0, 16, 12, 9};
        first_components = 3;
    }
    Shader::Backend::Bindings bindings{};
    Shader::Backend::SPIRV::EmitContext context(profile, runtime, program, bindings);
    for (u32 i = 0; i < first_components; ++i) {
        REQUIRE(context.output_generics[0][i].num_components == first_components);
        REQUIRE(context.output_generics[0][i].first_element == 0);
    }
    if (first_components == 3) {
        REQUIRE(context.output_generics[0][3].num_components == 1);
        REQUIRE(context.output_generics[0][3].first_element == 3);
    }
    for (const auto& component : context.output_generics[1]) {
        REQUIRE(component.num_components == 0);
    }
}

TEST_CASE("Product multiplicity counts unique modules and excludes dropped entries",
          "[video_core][translation_cache]") {
    Store store;
    store.Open({}, {11, 22});
    const auto a = store.Insert({1, 2}, {}, {.stage = Shader::Stage::Fragment});
    const auto b = store.Insert({3, 4}, {}, {.stage = Shader::Stage::Fragment});
    REQUIRE(store.VariantMultiplicity().at(0) == 2);
    const Variant variant{.spirv = {0x07230203, 1, 2, 3, 4}};
    REQUIRE(store.InsertVariant(*a, {5, 6}, variant));
    REQUIRE(store.InsertVariant(*a, {5, 6}, variant));
    REQUIRE(store.InsertVariant(*a, {7, 8}, variant));
    REQUIRE(store.VariantMultiplicity().at(2) == 1);
    REQUIRE(store.VariantMultiplicity().at(0) == 1);
    store.DropEntry(*a);
    REQUIRE_FALSE(store.HasVariant({5, 6}));
    const auto histogram = store.VariantMultiplicity();
    REQUIRE(histogram.size() == 1);
    REQUIRE(histogram.at(0) == 1);
}

TEST_CASE("Recipe import seeks past large offline products before runtime recipes",
          "[video_core][translation_cache]") {
    const auto path = TempPath("vulkan_translations.source-test.fork-v54.bin");
    std::filesystem::remove(path);
    const Digest config{101, 102}, key{103, 104};
    bool offline_entry = false;
    SECTION("runtime entries") {}
    SECTION("runtime recipes attached to offline entries") { offline_entry = true; }
    constexpr u32 payload_size = 100 * 1024 * 1024;
    {
        Store source;
        source.Open(path, config);
        REQUIRE(source.Insert({201, 202}, {}, {}, true));
    }
    // Sparse framed fallback modules: reading their irrelevant payload would allocate 300 MiB.
    {
        std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
        file.seekp(0, std::ios::end);
        const auto put = [&](const auto& value) {
            file.write(reinterpret_cast<const char*>(&value), sizeof(value));
        };
        for (u32 i = 0; i < 3; ++i) {
            put(u32{0x52435453});
            put(payload_size);
            const auto begin = file.tellp();
            put(u8{1});
            put(Digest{301, i});
            put(Digest{201, 202});
            put(true);
            file.seekp(begin + std::streamoff(payload_size - 1));
            put(u8{});
        }
    }
    // Append a valid runtime entry/recipes from another small file without loading the product.
    const auto tail = TempPath("lichii_recipe_tail.bin");
    std::filesystem::remove(tail);
    {
        Store source;
        source.Open(tail, config);
        const auto entry = source.Insert(key, {}, {.stage = Shader::Stage::Fragment}, offline_entry);
        for (u32 i = 1; i <= 3; ++i) {
            auto recipe = DefaultEmissionRecipe(Shader::Stage::Fragment);
            recipe.bindings.texture = i;
            REQUIRE(source.InsertVariant(*entry,
                MakeVariantKey(entry->id, DigestRuntimeInfo(recipe.runtime), recipe.bindings),
                {.spirv = {0x07230203, 1, 2}}, &recipe));
        }
        REQUIRE(source.Insert({401, 402}, {}, {})); // Entry-only record.
    }
    {
        std::ifstream input(tail, std::ios::binary);
        input.seekg(8 + sizeof(u32) + sizeof(Digest));
        std::ofstream output(path, std::ios::binary | std::ios::app);
        output << input.rdbuf();
    }
    Common::FS::SetEdenPath(Common::FS::EdenPath::LogDir, path.parent_path());
    Common::Log::Initialize();
    Common::Log::SetGlobalFilter(Common::Log::Filter{Common::Log::Level::Info});
    Common::Log::SetRingCapacity(64);
    const auto catalog = Store::ReadRecipeCatalog(path, {999, 999});
    const auto lines = Common::Log::ReadRing(0);
    REQUIRE(std::ranges::count_if(lines, [](const auto& line) {
        return line.text.find("source translator tag source-test into current tag " +
                              CurrentCacheIdentity().file_tag) != std::string::npos;
    }) == 1);
    Common::Log::SetRingCapacity(0);
    REQUIRE(catalog.source_config == config);
    REQUIRE(catalog.exact.at(key).size() == 3);
    REQUIRE(catalog.scanned_records == 9);
    REQUIRE(catalog.eligible_records == 3);
    REQUIRE(catalog.admitted_records == 3);
    REQUIRE(catalog.skipped_records == 0);
    REQUIRE(catalog.imported_bytes < 16384);
    REQUIRE(catalog.framing_skipped_bytes > 299ULL * 1024 * 1024);
    const auto limited = Store::ReadRecipeCatalog(path, config, 200000, catalog.imported_bytes - 1);
    REQUIRE(limited.admitted_records == 2);
    REQUIRE(limited.skipped_records == 1);
    REQUIRE(limited.eligible_records == 3);
    REQUIRE(limited.imported_bytes < catalog.imported_bytes);
    std::filesystem::remove(path);
    std::filesystem::remove(tail);
}

TEST_CASE("Translation cache open names other translator tags once",
          "[video_core][translation_cache]") {
    const auto directory = TempPath("lichii_translation_tag_test");
    std::filesystem::create_directories(directory);
    const auto current = CurrentCacheIdentity().file_tag;
    for (const auto* tag : {"old-tag-a", "old-tag-b"}) {
        std::ofstream(directory / (std::string{"vulkan_translations."} + tag + ".fork-v54.bin"));
    }
    Common::FS::SetEdenPath(Common::FS::EdenPath::LogDir, directory);
    Common::Log::Initialize();
    Common::Log::SetGlobalFilter(Common::Log::Filter{Common::Log::Level::Info});
    Common::Log::SetRingCapacity(64);
    {
        Store store;
        store.Open(directory / ("vulkan_translations." + current + ".fork-v54.bin"), {1, 2});
    }
    const auto lines = Common::Log::ReadRing(0);
    REQUIRE(std::ranges::count_if(lines, [&](const auto& line) {
        return line.text.find("ignoring files with translator tag(s) old-tag-a, old-tag-b; current tag " + current) != std::string::npos;
    }) == 1);
    Common::Log::SetRingCapacity(0);
    std::filesystem::remove_all(directory);
}

TEST_CASE("Translation cache keeps appended variants resident and promotes loaded variants",
          "[video_core][translation_cache]") {
    const auto path = TempPath("lichii_translation_resident_test.bin");
    std::filesystem::remove(path);
    const Digest config{11, 12}, key{13, 14};
    const Variant variant{.info = SampleInfo(), .spirv = {0x07230203, 1, 2, 3}};
    {
        Store store;
        store.Open(path, config);
        REQUIRE(store.Persistent());
        const auto entry = store.Insert({15, 16}, {}, ProgramMeta{});
        REQUIRE(entry);
        REQUIRE(store.InsertVariant(*entry, key, variant));
        REQUIRE(store.ResidentBytes() > 0);
        for (int i = 0; i < 2; ++i) {
            const auto found = store.FindVariant(key);
            REQUIRE(found);
            REQUIRE(InfoEqual(found->info, variant.info));
            REQUIRE(found->spirv == variant.spirv);
        }
        REQUIRE(store.variant_disk_reads == 0);
    }
    {
        Store store;
        store.Open(path, config);
        REQUIRE(store.ResidentBytes() == 0);
        const auto first = store.FindVariant(key);
        REQUIRE(first);
        REQUIRE(InfoEqual(first->info, variant.info));
        REQUIRE(first->spirv == variant.spirv);
        REQUIRE(store.variant_disk_reads == 1);
        REQUIRE(store.ResidentBytes() > 0);
        const auto second = store.FindVariant(key);
        REQUIRE(second);
        REQUIRE(InfoEqual(second->info, variant.info));
        REQUIRE(second->spirv == variant.spirv);
        REQUIRE(store.variant_disk_reads == 1);
    }
    std::filesystem::remove(path);
}

TEST_CASE("Translation cache evicts oldest resident variants only with a disk copy",
          "[video_core][translation_cache]") {
    const auto path = TempPath("lichii_translation_resident_budget_test.bin");
    std::filesystem::remove(path);
    const Digest config{21, 22}, first_key{23, 24}, second_key{25, 26};
    const Variant variant{.info = SampleInfo(), .spirv = {0x07230203, 1, 2, 3}};
    u64 one_variant_bytes{};
    {
        Store memory;
        memory.Open({}, config);
        const auto entry = memory.Insert({27, 28}, {}, ProgramMeta{});
        REQUIRE(entry);
        REQUIRE(memory.InsertVariant(*entry, first_key, variant));
        one_variant_bytes = memory.ResidentBytes();
    }
    REQUIRE(one_variant_bytes > 0);
    {
        Store store{one_variant_bytes};
        store.Open(path, config);
        REQUIRE(store.Persistent());
        const auto entry = store.Insert({27, 28}, {}, ProgramMeta{});
        REQUIRE(entry);
        REQUIRE(store.InsertVariant(*entry, first_key, variant));
        REQUIRE(store.InsertVariant(*entry, second_key, variant));
        REQUIRE(store.ResidentBytes() <= one_variant_bytes);
        REQUIRE(store.FindVariant(second_key));
        REQUIRE(store.variant_disk_reads == 0);
        const auto first = store.FindVariant(first_key);
        REQUIRE(first);
        REQUIRE(InfoEqual(first->info, variant.info));
        REQUIRE(first->spirv == variant.spirv);
        REQUIRE(store.variant_disk_reads == 1);
        REQUIRE(store.ResidentBytes() <= one_variant_bytes);
        REQUIRE(store.FindVariant(first_key));
        REQUIRE(store.variant_disk_reads == 1);
        REQUIRE(store.FindVariant(second_key));
        REQUIRE(store.variant_disk_reads == 2);
    }
    {
        Store store{0};
        store.Open(path, config);
        REQUIRE(store.FindVariant(first_key));
        REQUIRE(store.ResidentBytes() == 0);
        REQUIRE(store.FindVariant(first_key));
        REQUIRE(store.variant_disk_reads == 2);
    }
    {
        Store memory{0};
        memory.Open({}, config);
        const auto entry = memory.Insert({27, 28}, {}, ProgramMeta{});
        REQUIRE(entry);
        REQUIRE(memory.InsertVariant(*entry, first_key, variant));
        REQUIRE(memory.ResidentBytes() > 0);
        REQUIRE(memory.FindVariant(first_key));
        REQUIRE(memory.variant_disk_reads == 0);
        memory.Close();
        REQUIRE(memory.ResidentBytes() == 0);
    }
    std::filesystem::remove(path);
}
