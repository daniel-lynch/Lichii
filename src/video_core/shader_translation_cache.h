// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/common_types.h"
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"
#include "shader_recompiler/shader_info.h"

// Translated shader cache: the result of translating one shader stage (Maxwell decode, IR passes
// and SPIR-V emission) kept in memory and on disk, so a pipeline built again (at boot, after
// eviction, or with new fixed state over known shaders) skips the translation.
//
// A stage entry is found by the stage key (the program's hash and every environment value the
// translator reads up front) and then validated by replaying the environment queries the
// translation made (constant buffer words, texture types, ...) against the live environment; the
// translator is deterministic, so equal answers mean an equal program. A variant of the entry holds
// the SPIR-V for one runtime info (the pipeline state the emitter reads) and descriptor binding.
namespace VideoCommon::TranslationCache {

struct Digest {
    u64 lo{};
    u64 hi{};

    bool operator==(const Digest&) const = default;
};

struct DigestHash {
    size_t operator()(const Digest& digest) const noexcept {
        return static_cast<size_t>(digest.lo ^ (digest.hi * 0x9e3779b97f4a7c15ULL));
    }
};

/// Collects bytes for a digest. Only values without padding are added.
class Hasher {
public:
    template <typename T>
    void Add(const T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        static_assert(std::has_unique_object_representations_v<T> || std::is_floating_point_v<T>,
                      "Values with padding would make digests depend on uninitialized bytes");
        AddBytes(&value, sizeof(value));
    }
    void AddBytes(const void* data, size_t size);
    void AddString(std::string_view text);
    [[nodiscard]] Digest Finish() const;

private:
    std::vector<u8> bytes;
};

enum class QueryKind : u8 {
    CbufValue,
    TextureType,
    TexturePixelFormat,
    TexturePixelFormatInteger,
    ViewportTransformState,
    CbufStorageBufferPointer,
    ReplaceConstBuffer,
    CbufIndirectStorageBufferPointer,
    TextureHandle,
};

struct Query {
    QueryKind kind{};
    u32 a{};
    u32 b{};
    u32 answer{};
    u32 c{}; ///< Third argument (the indirect offset of CbufIndirectStorageBufferPointer)

    bool operator==(const Query&) const = default;
};

/// Forwards to a live environment and records each query the translator makes with its answer.
class RecordingEnvironment final : public Shader::Environment {
public:
    explicit RecordingEnvironment(Shader::Environment& inner, bool class_replay = false);
    void BeginTextureHandle() override;
    void EndTextureHandle(const Shader::ConstBufferAddr& cbuf, u32 handle) override;
    void NoteExactCbufWord(u32 index, u32 offset) override;
    ~RecordingEnvironment() override = default;

    u64 ReadInstruction(u32 address) override;
    u32 TranslateProgramAddress(u32 address) const override;
    bool IsNativeProgram() const override;
    bool IsCbufStorageBufferPointer(u32 index, u32 offset) override;
    bool IsCbufIndirectStorageBufferPointer(u32 index, u32 offset, u32 indirect) override;
    u32 ReadCbufValue(u32 cbuf_index, u32 cbuf_offset) override;
    Shader::TextureType ReadTextureType(u32 raw_handle) override;
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32 raw_handle) override;
    bool IsTexturePixelFormatInteger(u32 raw_handle) override;
    u32 ReadViewportTransformState() override;
    u32 TextureBoundBuffer() const override;
    u32 LocalMemorySize() const override;
    u32 SharedMemorySize() const override;
    std::array<u32, 3> WorkgroupSize() const override;
    bool HasHLEMacroState() const override;
    bool HasNativeInstructions() const override;
    bool CanCacheTranslation() const override;
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32 bank, u32 offset) override;
    void Dump(u64 pipeline_hash, u64 shader_hash) override;

    [[nodiscard]] std::vector<Query> TakeQueries();

private:
    Shader::Environment& inner;
    std::vector<Query> queries;
    bool class_replay{};
    u32 handle_depth{};
    std::unordered_set<u64> handle_words;
    std::unordered_set<u64> exact_words;
};

/// Asks the live environment the recorded queries in order; true when every answer matches.
/// Stops at the first mismatch, so it never makes a query the translation would not make.
[[nodiscard]] bool ReplayQueries(std::span<const Query> queries, Shader::Environment& env);

/// Key of a stage: its hash and the environment values the translator reads besides queries.
[[nodiscard]] Digest MakeStageKey(Shader::Environment& env, u64 unique_hash, u32 slot,
                                  const Digest& config);

/// What identifies the translator in the configuration digest and the file name.
struct CacheIdentity {
    /// "source": Shader::TranslatorIdentity(), a hash of the translator's source and of the build
    /// facts that can change its output, computed at build time. A rebuild that leaves the
    /// translator alone keeps the cache, and other executables built from the same source agree.
    /// "exe" (EDEN_NX2_TRANSLATION_CACHE_KEY=exe): the commit and the executable's size and
    /// modification time, as before; every build starts a new cache.
    std::string_view kind;
    /// The value folded into the configuration digest; empty when the build cannot be identified.
    std::string value;
    /// 16 hexadecimal digits for the file name (vulkan_translations.<tag>.fork-vN.bin), so files
    /// of different translators sit side by side. Empty for "exe", which keeps the old name.
    std::string file_tag;
    /// One line for the log.
    std::string description;
};

[[nodiscard]] CacheIdentity CurrentCacheIdentity();

/// Digest of what translation and emission depend on beyond the environment: the translator's
/// identity, the profile, the host info, the settings and environment variables the recompiler
/// reads, and an extra value from the caller (device limits it applies itself).
[[nodiscard]] Digest MakeConfigDigest(const Shader::Profile& profile,
                                      const Shader::HostTranslateInfo& host_info, u64 extra);

[[nodiscard]] Digest DigestRuntimeInfo(const Shader::RuntimeInfo& runtime_info);

/// A program without its IR: what pipelines and later stages read from a translated program.
struct ProgramMeta {
    Shader::Stage stage{};
    std::array<u32, 3> workgroup_size{};
    Shader::OutputTopology output_topology{};
    u32 output_vertices{};
    u32 invocations{};
    u32 local_memory_size{};
    u32 shared_memory_size{};
    bool is_geometry_passthrough{};
    Shader::Info info;

    [[nodiscard]] static ProgramMeta From(const Shader::IR::Program& program);
    /// A program with these fields and no IR blocks.
    [[nodiscard]] Shader::IR::Program ToProgram() const;
};

struct Entry {
    Digest stage_key;
    std::vector<Query> queries;
    ProgramMeta meta;
    /// Digest of the stage key and the queries: identifies the program the entry translates.
    Digest id;
};

struct Variant {
    Shader::Backend::Bindings binding_out{};
    Shader::Info info;
    std::vector<u32> spirv;
};

[[nodiscard]] Digest MakeVariantKey(const Digest& entry_id, const Digest& runtime,
                                    const Shader::Backend::Bindings& binding_in);

/// Field-by-field equality of two infos (byte comparison would include struct padding).
[[nodiscard]] bool InfoEqual(const Shader::Info& lhs, const Shader::Info& rhs);

/// Serialization of the shader info, exposed for tests.
void WriteInfo(std::vector<u8>& out, const Shader::Info& info);
[[nodiscard]] bool ReadInfo(std::span<const u8>& in, Shader::Info& info);

/// False when Shader::Info (or another serialized structure) gained a field this file does not
/// write yet; the cache is then off rather than handing pipelines an incomplete info.
[[nodiscard]] bool LayoutIsCovered();

class Store {
public:
    Store() = default;
    ~Store();

    /// Loads the cache file of a title, or starts a new one when its build or configuration
    /// differs. An empty path keeps the cache in memory only.
    void Open(const std::filesystem::path& path, const Digest& config);
    void Close();

    [[nodiscard]] bool Enabled() const noexcept {
        return enabled;
    }

    /// The entry of a stage whose recorded queries the environment answers the same way.
    [[nodiscard]] std::shared_ptr<const Entry> FindEntry(const Digest& stage_key,
                                                         Shader::Environment& env);
    /// The variant, decompressed.
    [[nodiscard]] std::optional<Variant> FindVariant(const Digest& variant_key);

    /// Adds an entry (when new) and a variant, and appends both to the file.
    std::shared_ptr<const Entry> Insert(const Digest& stage_key, std::vector<Query> queries,
                                        const ProgramMeta& meta);
    void InsertVariant(const Entry& entry, const Digest& variant_key, const Variant& variant);

    void LogStatistics(std::string_view when);

    // Statistics
    std::atomic<u64> stage_hits{};
    std::atomic<u64> stage_misses{};
    std::atomic<u64> hit_ns{};
    std::atomic<u64> miss_ns{};

private:
    struct StoredVariant {
        Shader::Backend::Bindings binding_out{};
        Shader::Info info;
        u32 spirv_words{};
        std::vector<u8> compressed_spirv;
    };

    std::shared_ptr<const Entry> InsertLocked(Entry entry);
    void Append(const std::vector<u8>& record);
    bool LoadFile(const std::filesystem::path& path, const Digest& config);

    std::mutex mutex;
    bool enabled{};
    std::unordered_map<Digest, std::vector<std::shared_ptr<const Entry>>, DigestHash> entries;
    std::unordered_map<Digest, std::shared_ptr<const StoredVariant>, DigestHash> variants;
    std::filesystem::path filename;
    std::ofstream file;
    u64 loaded_entries{};
    u64 loaded_variants{};
};

} // namespace VideoCommon::TranslationCache
