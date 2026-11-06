// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
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
    /// The texture handle built from constant buffer words (class-level replay only): a, b and c
    /// pack the ConstBufferAddr (PackHandleAddress), the answer is the handle. Replay computes
    /// the live handle and compares the texture queries made with it, not the words.
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
    /// With class_replay, a constant buffer word consumed only as part of a texture handle is
    /// recorded as the handle (QueryKind::TextureHandle) instead of its raw value.
    explicit RecordingEnvironment(Shader::Environment& inner, bool class_replay = false);
    ~RecordingEnvironment() override = default;

    void BeginTextureHandle() override;
    void EndTextureHandle(const Shader::ConstBufferAddr& cbuf, u32 handle) override;
    void NoteExactCbufWord(u32 index, u32 offset) override;

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

/// Live answers gathered by a class-level replay, for retranslating the stage elsewhere: the
/// queries with the live environment's words, handles and answers.
using LiveAnswers = std::vector<Query>;

/// Asks the live environment the recorded queries in order; true when every answer matches.
/// Stops at the first mismatch, so it never makes a query the translation would not make.
[[nodiscard]] bool ReplayQueries(std::span<const Query> queries, Shader::Environment& env);
/// ReplayQueries that also says where it stopped: the index of the first query answered
/// differently (queries.size() when all matched).
[[nodiscard]] size_t ReplayQueriesUntilMismatch(std::span<const Query> queries,
                                                Shader::Environment& env,
                                                LiveAnswers* live = nullptr);

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
[[nodiscard]] Shader::RuntimeInfo CanonicalRuntimeInfo(Shader::RuntimeInfo runtime,
                                                      const Shader::Info& info,
                                                      Shader::Stage stage);

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
    Shader::MeshInfo mesh;

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
    bool offline{};

    [[nodiscard]] bool CanReuseMetadata() const { return true; }
};

struct Variant {
    Shader::Backend::Bindings binding_out{};
    Shader::Info info;
    std::vector<u32> spirv;
    bool fallback_recipe{};
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

struct EmissionRecipe {
    Shader::RuntimeInfo runtime{};
    Shader::Backend::Bindings bindings{};
};

[[nodiscard]] EmissionRecipe DefaultEmissionRecipe(Shader::Stage stage);

[[nodiscard]] u64 RecipeClass(Shader::Stage stage, const Shader::Info* info);

constexpr u32 DefaultRecipeLimit = 200000;
constexpr u64 DefaultRecipeImportBytes = 256ULL * 1024 * 1024;

constexpr u32 DefaultRecipeCount = 2;
enum class RecipeBaseline { Auto, Always, Never };
enum class RecipeSource { ExactStage, Class, Baseline };
struct SelectedRecipe {
    EmissionRecipe recipe;
    RecipeSource source;
};

struct RecipeCatalog {
    struct CountedRecipe {
        EmissionRecipe recipe;
        Digest key;
        u64 count{};
    };
    Digest source_config{};
    u64 entries{}, truncated_entries{}, skipped_records{}, imported_bytes{}, invalid_recipes{};
    std::unordered_map<Digest, std::vector<CountedRecipe>, DigestHash> exact;
    std::unordered_map<u64, std::vector<CountedRecipe>> classes;
    [[nodiscard]] std::vector<SelectedRecipe> SelectWithSources(
        const Digest& stage_key, Shader::Stage stage, const Shader::Info& info, u32 top_k = DefaultRecipeCount,
        RecipeBaseline baseline = RecipeBaseline::Auto) const;
    [[nodiscard]] std::vector<EmissionRecipe> Select(
        const Digest& stage_key, Shader::Stage stage, const Shader::Info& info,
        u32 top_k = DefaultRecipeCount,
        RecipeBaseline baseline = RecipeBaseline::Auto) const;
};

enum class OfflineOutcome { VariantHit, VariantMiss, MetaMismatch, FullHit };

class Store {
public:
    Store() = default;
    ~Store();

    /// Loads the cache file of a title, or starts a new one when its build or configuration
    /// differs. An empty path keeps the cache in memory only.
    void Open(const std::filesystem::path& path, const Digest& config);
    void Close();
    [[nodiscard]] bool Persistent();

    [[nodiscard]] bool Enabled() const noexcept {
        return enabled;
    }

    /// The entry of a stage whose recorded queries the environment answers the same way.
    /// TextureHandle queries replay by class; optional `live` captures the matching entry's
    /// current answers for worker-side reconstruction.
    [[nodiscard]] std::shared_ptr<const Entry> FindEntry(const Digest& stage_key,
                                                         Shader::Environment& env,
                                                         LiveAnswers* live = nullptr);
    /// True when entries exist for the stage key (whether or not the environment matches them).
    [[nodiscard]] bool HasStageKey(const Digest& stage_key);
    /// Current non-dropped entries grouped by their number of stored modules.
    [[nodiscard]] std::map<u64, u64> VariantMultiplicity();
    /// The variant, decompressed.
    [[nodiscard]] bool HasVariant(const Digest& variant_key);
    [[nodiscard]] std::optional<Variant> FindVariant(const Digest& variant_key);

    /// Adds an entry (when new) and appends it to the file. With `expected_generation`, nothing
    /// is added unless the store is still the one opened then (checked under the store mutex).
    std::shared_ptr<const Entry> Insert(const Digest& stage_key, std::vector<Query> queries,
                                        const ProgramMeta& meta, bool offline = false,
                                        std::optional<u64> expected_generation = std::nullopt);
    std::shared_ptr<const Entry> Insert(const Digest& stage_key, std::vector<Query> queries,
                                        const ProgramMeta& meta, u64 expected_generation) {
        return Insert(stage_key, std::move(queries), meta, false, expected_generation);
    }
    /// Adds a variant only if its entry still belongs to this store.
    bool InsertVariant(const Entry& entry, const Digest& variant_key, const Variant& variant,
                       const EmissionRecipe* recipe = nullptr);
    [[nodiscard]] u64 Generation() const noexcept {
        return generation.load();
    }
    std::atomic<u64> rejected_variants{};

    /// Reads recipes without modifying the source cache, including an incomplete tail.
    [[nodiscard]] static std::array<EmissionRecipe, Shader::MaxStageTypes> ReadRecipes(
        const std::filesystem::path& path, const Digest& config);

    /// Read only recipes from any compatible format-10 configuration; never reuse its modules.
    [[nodiscard]] static RecipeCatalog ReadRecipeCatalog(const std::filesystem::path& path,
                                                        const Digest& config,
                                                        u32 entry_limit = DefaultRecipeLimit,
                                                        u64 byte_budget = DefaultRecipeImportBytes);

    /// Reject speculative metadata before any pipeline state is derived from it.
    [[nodiscard]] bool ValidateOfflineEntry(const Entry& entry, const Shader::IR::Program& live);

    void DropEntry(const Entry& entry);
    void LogOfflineOutcome(Shader::Stage stage, u64 hash, OfflineOutcome outcome);
    void LogStatistics(std::string_view when);

    /// With count_mismatches set (use_shader_aot_translation), FindEntry misses on a known
    /// stage key are counted by the kind of the first query answered differently, taken from the
    /// candidate whose replay got furthest.
    bool count_mismatches{};
    std::array<std::atomic<u64>, 16> first_mismatch{};

    // Statistics
    std::array<std::array<std::atomic<u64>, 4>, Shader::MaxStageTypes> offline_outcomes{};
    std::atomic<u64> offline_entry_only_rejected{};
    std::atomic<u64> entry_key_misses{}, offline_replay_misses{}, offline_dropped_misses{};
    std::atomic<u64> stage_hits{};
    std::atomic<u64> stage_misses{};
    std::atomic<u64> hit_ns{};
    std::atomic<u64> miss_ns{};

private:
    struct StoredVariant {
        Digest entry_id{};
        bool fallback_recipe{};
        Shader::Backend::Bindings binding_out{};
        std::unique_ptr<Shader::Info> info;
        u64 info_offset{};
        u32 info_size{};
        u32 spirv_words{};
        std::vector<u8> compressed_spirv;
        std::filesystem::path source;
        u64 compressed_offset{};
        u32 compressed_size{};
    };

    std::shared_ptr<const Entry> InsertLocked(Entry entry);
    void Append(const std::vector<u8>& record);
    void DisablePersistence(const std::error_code& error);
    bool LoadFile(const std::filesystem::path& path, const Digest& config, bool writable = true,
                  u64 byte_budget = UINT64_MAX);

    struct RecipeCount {
        EmissionRecipe recipe;
        Shader::Stage stage;
        u64 count{};
    };
    void CountRecipe(Shader::Stage stage, const EmissionRecipe& recipe);
    std::unordered_map<Digest, RecipeCount, DigestHash> recipes;

    bool collect_recipe_entries{};
    u64 skipped_import_records{}, imported_bytes{}, invalid_recipes{};
    std::unordered_map<Digest, std::vector<EmissionRecipe>, DigestHash> entry_recipes;

    // Writers serialize publication and append; readers never wait for file I/O.
    std::mutex writer_mutex;
    std::mutex mutex;
    bool enabled{};
    bool persistence_failed{};
    std::atomic<u64> generation{};
    std::unordered_map<Digest, std::vector<std::shared_ptr<const Entry>>, DigestHash> entries;
    std::unordered_map<Digest, std::shared_ptr<const StoredVariant>, DigestHash> variants;
    std::unordered_set<Digest, DigestHash> dropped_entries;
    std::unordered_set<Digest, DigestHash> entries_with_modules;
    std::filesystem::path filename;
    std::ofstream file;
    u64 loaded_entries{};
    u64 loaded_variants{};
};

} // namespace VideoCommon::TranslationCache
