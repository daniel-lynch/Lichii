// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <system_error>
#include <type_traits>
#include <utility>

#include "common/cityhash.h"
#include "common/fs/fs.h"
#include "common/logging.h"
#include "common/scm_rev.h"
#include "common/settings.h"
#include "common/zstd_compression.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/translator_identity.h"
#include "video_core/shader_translation_cache.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace VideoCommon::TranslationCache {
namespace {

// Bump when the file layout or what a key covers changes.
// 2: StorageBufferDescriptor gained write_extent (records store descriptors as raw structures).
// 3: StorageBufferDescriptor gained native_indirect, Info native_indirect_slots.
// 4: StorageBufferDescriptor gained indirect_offset (upstream nx2 #150, merged at 27226d2568).
// 5: IsCbufIndirectStorageBufferPointer is recorded and replayed (queries gained a third
//    argument); before, the recording environment answered it with the base class's `true`.
// 6: class-level replay (use_shader_aot_translation) records texture handles by their source
//    words (QueryKind::TextureHandle) instead of the raw handle words.
// 9: the class-level replay records (6) on top of the producer branch's 7-8. The translator's
//    identity (CurrentCacheIdentity) is not part of the layout: it is folded into the
//    configuration digest and named in the file name, so it needs no version of its own.
constexpr u32 FormatVersion = 9;
constexpr std::array<char, 8> FileMagic{'L', 'X', 'T', 'R', 'A', 'N', 'S', 'C'};
constexpr u32 RecordMagic = 0x52435453; // "STCR"
constexpr u8 RecordEntry = 0;
constexpr u8 RecordVariant = 1;

// The number of fields of an aggregate, to notice a structure gaining a field this file does not
// serialize or digest yet.
struct AnyField {
    template <typename T>
    operator T() const;
};

template <typename T, size_t... I>
consteval bool BraceConstructible(std::index_sequence<I...>) {
    return requires { T{(void(I), AnyField{})...}; };
}

template <typename T, size_t N = 0>
consteval size_t CountFields() {
    if constexpr (N > 160) {
        return N;
    } else if constexpr (BraceConstructible<T>(std::make_index_sequence<N + 1>{})) {
        return CountFields<T, N + 1>();
    } else {
        return N;
    }
}

// Field lists of the structures written or digested here (field order of the declarations).
#define TRANSLATION_CACHE_INFO_FIELDS(POD, MAP, VEC, VECVEC)                                      \
    POD(is_task) POD(is_mesh) POD(uses_workgroup_id) POD(uses_local_invocation_id)                 \
    POD(uses_invocation_id) POD(uses_invocation_info) POD(uses_sample_id)                          \
    POD(uses_is_helper_invocation) POD(uses_subgroup_invocation_id) POD(uses_subgroup_shuffles)   \
    POD(uses_quad_shuffles) POD(uses_patches) POD(interpolation) POD(loads) POD(stores)           \
    POD(passthrough) MAP(legacy_stores_mapping) POD(loads_indexed_attributes)                      \
    POD(stores_frag_color) POD(stores_sample_mask) POD(stores_frag_depth)                          \
    POD(stores_tess_level_outer) POD(stores_tess_level_inner) POD(stores_indexed_attributes)      \
    POD(stores_global_memory) POD(uses_local_memory) POD(uses_fp16) POD(uses_fp64)                 \
    POD(uses_fp16_denorms_flush) POD(uses_fp16_denorms_preserve) POD(uses_fp32_denorms_flush)     \
    POD(uses_fp32_denorms_preserve) POD(uses_int8) POD(uses_int16) POD(uses_int64)                \
    POD(uses_image_1d) POD(uses_sampled_1d) POD(uses_sparse_residency)                             \
    POD(uses_demote_to_helper_invocation) POD(uses_subgroup_vote) POD(uses_subgroup_mask)         \
    POD(uses_fswzadd) POD(uses_derivatives) POD(uses_typeless_image_reads)                         \
    POD(uses_typeless_image_writes) POD(uses_image_buffers) POD(uses_shared_increment)            \
    POD(uses_shared_decrement) POD(uses_global_increment) POD(uses_global_decrement)              \
    POD(uses_atomic_f32_add) POD(uses_atomic_f16x2_add) POD(uses_atomic_f16x2_min)                 \
    POD(uses_atomic_f16x2_max) POD(uses_atomic_f32x2_add) POD(uses_atomic_f32x2_min)              \
    POD(uses_atomic_f32x2_max) POD(uses_atomic_s32_min) POD(uses_atomic_s32_max)                   \
    POD(uses_int64_bit_atomics) POD(uses_global_memory) POD(requires_coherent_global_memory)      \
    POD(requires_coherent_image_memory) POD(native_global_bounds) POD(native_global_stats)        \
    VEC(native_indirect_slots) POD(uses_atomic_image_u32)                                          \
    POD(uses_shadow_lod) POD(uses_rescaling_uniform) POD(uses_cbuf_indirect)                       \
    POD(uses_render_area) POD(used_constant_buffer_types) POD(used_storage_buffer_types)          \
    POD(used_indirect_cbuf_types) POD(constant_buffer_mask) POD(constant_buffer_used_sizes)       \
    POD(nvn_buffer_base) POD(nvn_buffer_used) POD(requires_layer_emulation)                        \
    POD(emulated_layer) POD(used_clip_distances) VEC(constant_buffer_descriptors)                  \
    VEC(storage_buffers_descriptors) VEC(texture_buffer_descriptors)                               \
    VEC(image_buffer_descriptors) VEC(texture_descriptors) VECVEC(handle_programs)                 \
    VEC(image_descriptors)
constexpr size_t InfoFields = 88;

#define TRANSLATION_CACHE_PROFILE_FIELDS(X) \
    X(supported_spirv) X(unified_descriptor_binding) X(support_descriptor_aliasing) \
    X(support_int8) X(support_uniform_and_storage_buffer_8bit) X(support_storage_buffer_8bit) \
    X(support_int16) X(support_uniform_and_storage_buffer_16bit) X(support_storage_buffer_16bit) \
    X(support_int64) X(support_vertex_instance_id) X(support_float_controls) \
    X(support_separate_denorm_behavior) X(support_separate_rounding_mode) \
    X(support_fp16_denorm_preserve) X(support_fp32_denorm_preserve) X(support_fp16_denorm_flush) \
    X(support_fp32_denorm_flush) X(support_fp16_signed_zero_nan_preserve) \
    X(support_fp32_signed_zero_nan_preserve) X(support_fp64_signed_zero_nan_preserve) \
    X(support_explicit_workgroup_layout) X(support_workgroup_layout_8bit_access) \
    X(support_workgroup_layout_16bit_access) X(support_shader_quad_control) \
    X(support_quad_shuffles) X(support_vote) X(support_shuffle_relative) \
    X(supported_subgroup_stages) X(support_viewport_index_layer_non_geometry) \
    X(support_viewport_mask) X(support_typeless_image_loads) \
    X(support_demote_to_helper_invocation) X(support_int64_atomics) \
    X(support_shared_int64_atomics) X(support_derivative_control) \
    X(support_geometry_shader_passthrough) X(support_native_ndc) X(support_gl_nv_gpu_shader_5) \
    X(support_gl_amd_gpu_shader_half_float) X(support_gl_texture_shadow_lod) \
    X(support_gl_warp_intrinsics) X(support_gl_variable_aoffi) X(support_gl_sparse_textures) \
    X(support_gl_derivative_control) X(support_scaled_attributes) X(support_multi_viewport) \
    X(support_geometry_streams) X(support_sampled_image_array_nonuniform_indexing) \
    X(support_storage_image_array_nonuniform_indexing) \
    X(support_uniform_texel_buffer_array_nonuniform_indexing) \
    X(support_storage_texel_buffer_array_nonuniform_indexing) \
    X(warp_size_potentially_larger_than_guest) X(lower_left_origin_mode) \
    X(need_declared_frag_colors) X(need_fastmath_off) X(need_gather_subpixel_offset) \
    X(has_broken_spirv_clamp) X(has_broken_spirv_position_input) \
    X(has_broken_unsigned_image_offsets) X(has_broken_signed_operations) \
    X(has_broken_fp16_float_controls) X(has_broken_fp32_denorm_flush) \
    X(has_gl_component_indexing_bug) X(has_gl_precise_bug) X(has_gl_cbuf_ftou_bug) \
    X(has_gl_bool_ref_bug) X(ignore_nan_fp_comparisons) \
    X(has_broken_spirv_subgroup_mask_vector_extract_dynamic) \
    X(has_broken_spirv_subgroup_shuffle) X(max_subgroup_size) X(gl_max_compute_smem_size) \
    X(has_broken_robust) X(min_ssbo_alignment) X(max_user_clip_distances)
constexpr size_t ProfileFields = 75;

constexpr size_t HostFields = 22;
constexpr size_t RuntimeInfoFields = 19;
constexpr size_t ProgramFields = 13;
constexpr size_t BindingsFields = 7;

class Writer {
public:
    explicit Writer(std::vector<u8>& out_) : out{out_} {}

    template <typename T>
    void Put(const T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        const size_t offset = out.size();
        out.resize(offset + sizeof(T));
        std::memcpy(out.data() + offset, &value, sizeof(T));
    }
    void PutBytes(const void* data, size_t size) {
        Put(static_cast<u32>(size));
        const size_t offset = out.size();
        out.resize(offset + size);
        if (size != 0) {
            std::memcpy(out.data() + offset, data, size);
        }
    }
    template <typename Container>
    void PutVector(const Container& values) {
        static_assert(std::is_trivially_copyable_v<typename Container::value_type>);
        Put(static_cast<u32>(values.size()));
        for (const auto& value : values) {
            Put(value);
        }
    }

private:
    std::vector<u8>& out;
};

class Reader {
public:
    explicit Reader(std::span<const u8>& in_) : in{in_} {}

    template <typename T>
    bool Get(T& value) {
        static_assert(std::is_trivially_copyable_v<T>);
        if (in.size() < sizeof(T)) {
            return false;
        }
        std::memcpy(&value, in.data(), sizeof(T));
        in = in.subspan(sizeof(T));
        return true;
    }
    bool GetBytes(std::vector<u8>& bytes) {
        u32 size{};
        if (!Get(size) || in.size() < size) {
            return false;
        }
        bytes.assign(in.begin(), in.begin() + size);
        in = in.subspan(size);
        return true;
    }
    template <typename Container>
    bool GetVector(Container& values, size_t capacity) {
        u32 size{};
        if (!Get(size) || size > capacity ||
            in.size() < size * sizeof(typename Container::value_type)) {
            return false;
        }
        values.clear();
        for (u32 i = 0; i < size; ++i) {
            typename Container::value_type value{};
            Get(value);
            values.push_back(value);
        }
        return true;
    }

private:
    std::span<const u8>& in;
};

constexpr size_t MaxElements = 1 << 20;

// static_vector's real capacity; small_vector's static_capacity is only its inline storage.
template <typename T>
size_t CapacityOf(const T& container) {
    return std::min<size_t>(container.max_size(), MaxElements);
}

// The old identity of the running build: the commit and the executable's size and modification
// time. Every build starts a new cache. Selected with EDEN_NX2_TRANSLATION_CACHE_KEY=exe.
std::string ExecutableIdentity() {
    std::string identity = fmt::format("{}|{}|{}", Common::g_scm_rev, Common::g_build_fullname,
                                       Common::g_compiler_id);
    std::filesystem::path exe;
#ifdef _WIN32
    wchar_t exe_path[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exe_path, MAX_PATH) != 0) {
        exe = exe_path;
    }
#elif defined(__linux__)
    exe = "/proc/self/exe";
#endif
    std::error_code ec;
    if (!exe.empty()) {
        const auto size = std::filesystem::file_size(exe, ec);
        if (!ec) {
            const auto time = std::filesystem::last_write_time(exe, ec);
            if (!ec) {
                return fmt::format("{}|{}|{}", identity, size,
                                   time.time_since_epoch().count());
            }
        }
    }
    return {};
}

void WriteQueries(Writer& writer, std::span<const Query> queries) {
    writer.Put(static_cast<u32>(queries.size()));
    for (const Query& query : queries) {
        writer.Put(query.kind);
        writer.Put(query.a);
        writer.Put(query.b);
        writer.Put(query.answer);
        writer.Put(query.c);
    }
}

bool ReadQueries(Reader& reader, std::vector<Query>& queries) {
    u32 count{};
    if (!reader.Get(count) || count > MaxElements) {
        return false;
    }
    queries.resize(count);
    for (Query& query : queries) {
        if (!reader.Get(query.kind) || !reader.Get(query.a) || !reader.Get(query.b) ||
            !reader.Get(query.answer) || !reader.Get(query.c)) {
            return false;
        }
    }
    return true;
}

void WriteMeta(std::vector<u8>& out, const ProgramMeta& meta) {
    Writer writer{out};
    writer.Put(meta.stage);
    writer.Put(meta.workgroup_size);
    writer.Put(meta.output_topology);
    writer.Put(meta.output_vertices);
    writer.Put(meta.invocations);
    writer.Put(meta.local_memory_size);
    writer.Put(meta.shared_memory_size);
    writer.Put(meta.is_geometry_passthrough);
    WriteInfo(out, meta.info);
    // Task and mesh entries (only stored with use_shader_aot_translation) carry the mesh sizes
    // and line maps the emitter and the next stage's runtime info read.
    if (meta.stage == Shader::Stage::Task || meta.stage == Shader::Stage::Mesh) {
        writer.Put(meta.mesh.payload_words);
        writer.Put(meta.mesh.max_vertices);
        writer.Put(meta.mesh.max_primitives);
        writer.Put(meta.mesh.topology);
        writer.PutVector(meta.mesh.vertex_lines);
        writer.PutVector(meta.mesh.primitive_lines);
    }
}

bool ReadMeta(std::span<const u8>& in, ProgramMeta& meta) {
    Reader reader{in};
    return reader.Get(meta.stage) && reader.Get(meta.workgroup_size) &&
           reader.Get(meta.output_topology) && reader.Get(meta.output_vertices) &&
           reader.Get(meta.invocations) && reader.Get(meta.local_memory_size) &&
           reader.Get(meta.shared_memory_size) && reader.Get(meta.is_geometry_passthrough) &&
           ReadInfo(in, meta.info) &&
           ((meta.stage != Shader::Stage::Task && meta.stage != Shader::Stage::Mesh) ||
            (reader.Get(meta.mesh.payload_words) && reader.Get(meta.mesh.max_vertices) &&
             reader.Get(meta.mesh.max_primitives) && reader.Get(meta.mesh.topology) &&
             reader.GetVector(meta.mesh.vertex_lines, meta.mesh.vertex_lines.capacity()) &&
             reader.GetVector(meta.mesh.primitive_lines, meta.mesh.primitive_lines.capacity())));
}

Digest EntryId(const Digest& stage_key, std::span<const Query> queries) {
    Hasher hasher;
    hasher.Add(stage_key.lo);
    hasher.Add(stage_key.hi);
    for (const Query& query : queries) {
        hasher.Add(query.kind);
        hasher.Add(query.a);
        hasher.Add(query.b);
        hasher.Add(query.answer);
        hasher.Add(query.c);
    }
    return hasher.Finish();
}

} // Anonymous namespace

void Hasher::AddBytes(const void* data, size_t size) {
    const auto* const begin = static_cast<const u8*>(data);
    bytes.insert(bytes.end(), begin, begin + size);
}

void Hasher::AddString(std::string_view text) {
    Add(static_cast<u64>(text.size()));
    AddBytes(text.data(), text.size());
}

Digest Hasher::Finish() const {
    const u128 hash = Common::CityHash128(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return Digest{hash[0], hash[1]};
}

namespace {
u64 WordKey(u32 index, u32 offset) {
    return (u64{index} << 32) | offset;
}

struct HandleAddress {
    u32 index, offset, shift_left, secondary_index, secondary_offset, secondary_shift_left;
    bool has_secondary;
};

Query PackHandleAddress(const Shader::ConstBufferAddr& cbuf, u32 handle) {
    return Query{
        .kind = QueryKind::TextureHandle,
        .a = (cbuf.index & 0xff) | ((cbuf.shift_left & 0xff) << 8) |
             ((cbuf.secondary_shift_left & 0xff) << 16) | (cbuf.has_secondary ? 1U << 24 : 0U),
        .b = cbuf.offset,
        .answer = handle,
        .c = ((cbuf.secondary_index & 0xffff) << 16) | (cbuf.secondary_offset & 0xffff),
    };
}

HandleAddress UnpackHandleAddress(const Query& query) {
    return HandleAddress{
        .index = query.a & 0xff,
        .offset = query.b,
        .shift_left = (query.a >> 8) & 0xff,
        .secondary_index = query.c >> 16,
        .secondary_offset = query.c & 0xffff,
        .secondary_shift_left = (query.a >> 16) & 0xff,
        .has_secondary = ((query.a >> 24) & 1) != 0,
    };
}
} // Anonymous namespace

RecordingEnvironment::RecordingEnvironment(Shader::Environment& inner_, bool class_replay_)
    : inner{inner_}, class_replay{class_replay_} {
    sph = inner.SPH();
    gp_passthrough_mask = inner.GpPassthroughMask();
    mesh_registers = inner.MeshRegisters();
    mesh_primitive_header = inner.MeshPrimitiveHeader();
    stage = inner.ShaderStage();
    start_address = inner.StartAddress();
    is_proprietary_driver = inner.IsProprietaryDriver();
}

u64 RecordingEnvironment::ReadInstruction(u32 address) {
    return inner.ReadInstruction(address);
}

u32 RecordingEnvironment::TranslateProgramAddress(u32 address) const {
    return inner.TranslateProgramAddress(address);
}

bool RecordingEnvironment::IsNativeProgram() const {
    return inner.IsNativeProgram();
}

bool RecordingEnvironment::IsCbufStorageBufferPointer(u32 index, u32 offset) {
    const bool answer = inner.IsCbufStorageBufferPointer(index, offset);
    queries.push_back({QueryKind::CbufStorageBufferPointer, index, offset, answer ? 1U : 0U});
    return answer;
}

bool RecordingEnvironment::IsCbufIndirectStorageBufferPointer(u32 index, u32 offset,
                                                              u32 indirect) {
    const bool answer = inner.IsCbufIndirectStorageBufferPointer(index, offset, indirect);
    queries.push_back(
        {QueryKind::CbufIndirectStorageBufferPointer, index, offset, answer ? 1U : 0U, indirect});
    return answer;
}

void RecordingEnvironment::BeginTextureHandle() {
    ++handle_depth;
    inner.BeginTextureHandle();
}

void RecordingEnvironment::EndTextureHandle(const Shader::ConstBufferAddr& cbuf, u32 handle) {
    if (handle_depth != 0) {
        --handle_depth;
    }
    inner.EndTextureHandle(cbuf, handle);
    if (class_replay) {
        queries.push_back(PackHandleAddress(cbuf, handle));
    }
}

void RecordingEnvironment::NoteExactCbufWord(u32 index, u32 offset) {
    exact_words.insert(WordKey(index, offset));
    inner.NoteExactCbufWord(index, offset);
}

std::vector<Query> RecordingEnvironment::TakeQueries() {
    if (class_replay) {
        // A word read only to build texture handles is covered by its TextureHandle queries;
        // a word also consumed by value keeps its exact query.
        std::erase_if(queries, [this](const Query& query) {
            const u64 key = WordKey(query.a, query.b);
            return query.kind == QueryKind::CbufValue && handle_words.contains(key) &&
                   !exact_words.contains(key);
        });
    }
    return std::move(queries);
}

u32 RecordingEnvironment::ReadCbufValue(u32 cbuf_index, u32 cbuf_offset) {
    (handle_depth != 0 ? handle_words : exact_words).insert(WordKey(cbuf_index, cbuf_offset));
    const u32 answer = inner.ReadCbufValue(cbuf_index, cbuf_offset);
    queries.push_back({QueryKind::CbufValue, cbuf_index, cbuf_offset, answer});
    return answer;
}

Shader::TextureType RecordingEnvironment::ReadTextureType(u32 raw_handle) {
    const auto answer = inner.ReadTextureType(raw_handle);
    queries.push_back({QueryKind::TextureType, raw_handle, 0, static_cast<u32>(answer)});
    return answer;
}

Shader::TexturePixelFormat RecordingEnvironment::ReadTexturePixelFormat(u32 raw_handle) {
    const auto answer = inner.ReadTexturePixelFormat(raw_handle);
    queries.push_back({QueryKind::TexturePixelFormat, raw_handle, 0, static_cast<u32>(answer)});
    return answer;
}

bool RecordingEnvironment::IsTexturePixelFormatInteger(u32 raw_handle) {
    const bool answer = inner.IsTexturePixelFormatInteger(raw_handle);
    queries.push_back({QueryKind::TexturePixelFormatInteger, raw_handle, 0, answer ? 1U : 0U});
    return answer;
}

u32 RecordingEnvironment::ReadViewportTransformState() {
    const u32 answer = inner.ReadViewportTransformState();
    queries.push_back({QueryKind::ViewportTransformState, 0, 0, answer});
    return answer;
}

u32 RecordingEnvironment::TextureBoundBuffer() const {
    return inner.TextureBoundBuffer();
}

u32 RecordingEnvironment::LocalMemorySize() const {
    return inner.LocalMemorySize();
}

u32 RecordingEnvironment::SharedMemorySize() const {
    return inner.SharedMemorySize();
}

std::array<u32, 3> RecordingEnvironment::WorkgroupSize() const {
    return inner.WorkgroupSize();
}

bool RecordingEnvironment::HasHLEMacroState() const {
    return inner.HasHLEMacroState();
}

bool RecordingEnvironment::HasNativeInstructions() const {
    return inner.HasNativeInstructions();
}

std::optional<Shader::ReplaceConstant> RecordingEnvironment::GetReplaceConstBuffer(u32 bank,
                                                                                  u32 offset) {
    const auto answer = inner.GetReplaceConstBuffer(bank, offset);
    queries.push_back({QueryKind::ReplaceConstBuffer, bank, offset,
                       answer ? static_cast<u32>(*answer) : ~0U});
    return answer;
}

bool RecordingEnvironment::CanCacheTranslation() const {
    return inner.CanCacheTranslation();
}

void RecordingEnvironment::Dump(u64 pipeline_hash, u64 shader_hash) {
    inner.Dump(pipeline_hash, shader_hash);
}

bool ReplayQueries(std::span<const Query> queries, Shader::Environment& env) {
    return ReplayQueriesUntilMismatch(queries, env) == queries.size();
}

size_t ReplayQueriesUntilMismatch(std::span<const Query> queries, Shader::Environment& env,
                                  LiveAnswers* live) {
    // Class-level replay: recorded handle -> live handle.
    std::unordered_map<u32, u32> handles;
    if (live) {
        live->clear();
    }
    for (size_t index = 0; index < queries.size(); ++index) {
        const Query& query = queries[index];
        if (query.kind == QueryKind::TextureHandle) {
            if (!live) {
                return index;
            }
            const HandleAddress cbuf = UnpackHandleAddress(query);
            const u32 sec_index = cbuf.has_secondary ? cbuf.secondary_index : cbuf.index;
            const u32 sec_offset = cbuf.has_secondary ? cbuf.secondary_offset : cbuf.offset;
            const u32 lhs_word = env.ReadCbufValue(cbuf.index, cbuf.offset);
            const u32 rhs_word = env.ReadCbufValue(sec_index, sec_offset);
            const u32 handle =
                (lhs_word << cbuf.shift_left) | (rhs_word << cbuf.secondary_shift_left);
            if (const auto [it, inserted] = handles.try_emplace(query.answer, handle);
                !inserted && it->second != handle) {
                // One recorded handle standing for two live ones: the texture queries cannot be
                // told apart.
                return index;
            }
            live->push_back({QueryKind::CbufValue, cbuf.index, cbuf.offset, lhs_word});
            live->push_back({QueryKind::CbufValue, sec_index, sec_offset, rhs_word});
            continue;
        }
        const bool texture_query = query.kind == QueryKind::TextureType ||
                                   query.kind == QueryKind::TexturePixelFormat ||
                                   query.kind == QueryKind::TexturePixelFormatInteger;
        u32 argument = query.a;
        if (live && texture_query) {
            if (const auto it = handles.find(query.a); it != handles.end()) {
                argument = it->second;
            }
        }
        u32 answer{};
        switch (query.kind) {
        case QueryKind::CbufValue:
            answer = env.ReadCbufValue(query.a, query.b);
            break;
        case QueryKind::TextureType:
            answer = static_cast<u32>(env.ReadTextureType(argument));
            break;
        case QueryKind::TexturePixelFormat:
            answer = static_cast<u32>(env.ReadTexturePixelFormat(argument));
            break;
        case QueryKind::TexturePixelFormatInteger:
            answer = env.IsTexturePixelFormatInteger(argument) ? 1U : 0U;
            break;
        case QueryKind::ViewportTransformState:
            answer = env.ReadViewportTransformState();
            break;
        case QueryKind::CbufStorageBufferPointer:
            answer = env.IsCbufStorageBufferPointer(query.a, query.b) ? 1U : 0U;
            break;
        case QueryKind::CbufIndirectStorageBufferPointer:
            answer = env.IsCbufIndirectStorageBufferPointer(query.a, query.b, query.c) ? 1U : 0U;
            break;
        case QueryKind::ReplaceConstBuffer: {
            const auto value = env.GetReplaceConstBuffer(query.a, query.b);
            answer = value ? static_cast<u32>(*value) : ~0U;
            break;
        }
        default:
            return index;
        }
        if (answer != query.answer) {
            return index;
        }
        if (live) {
            Query live_query = query;
            live_query.a = argument;
            live->push_back(live_query);
        }
    }
    return queries.size();
}

Digest MakeStageKey(Shader::Environment& env, u64 unique_hash, u32 slot, const Digest& config) {
    Hasher hasher;
    hasher.Add(config.lo);
    hasher.Add(config.hi);
    hasher.Add(unique_hash);
    hasher.Add(slot);
    hasher.Add(env.ShaderStage());
    hasher.Add(env.StartAddress());
    hasher.AddBytes(&env.SPH(), sizeof(Shader::ProgramHeader));
    hasher.Add(env.GpPassthroughMask());
    hasher.Add(env.MeshRegisters());
    hasher.AddBytes(&env.MeshPrimitiveHeader(), sizeof(Shader::ProgramHeader));
    hasher.Add(env.IsProprietaryDriver());
    hasher.Add(env.TextureBoundBuffer());
    hasher.Add(env.LocalMemorySize());
    hasher.Add(env.SharedMemorySize());
    hasher.Add(env.WorkgroupSize());
    hasher.Add(env.HasHLEMacroState());
    hasher.Add(env.IsNativeProgram());
    hasher.Add(env.HasNativeInstructions());
    return hasher.Finish();
}

CacheIdentity CurrentCacheIdentity() {
    const char* const variable = std::getenv("EDEN_NX2_TRANSLATION_CACHE_KEY");
    const std::string_view requested = variable != nullptr ? variable : "";
    if (requested == "exe") {
        std::string executable = ExecutableIdentity();
        if (executable.empty()) {
            return {"exe", {}, {},
                    "exe (EDEN_NX2_TRANSLATION_CACHE_KEY=exe), but the executable cannot be read"};
        }
        return {"exe", "exe|" + executable, {},
                fmt::format("exe (EDEN_NX2_TRANSLATION_CACHE_KEY=exe, commit {}): every build "
                            "starts a new cache",
                            Common::g_scm_rev)};
    }
    std::string value = Shader::TranslatorIdentity();
    Hasher hasher;
    hasher.AddString(value);
    const Digest digest = hasher.Finish();
    std::string file_tag = fmt::format("{:016x}", digest.lo);
    std::string description = fmt::format("source {} ({} translator files, file tag {})",
                                          Shader::TranslatorSourceHash().substr(0, 16),
                                          Shader::TranslatorSourceFileCount(), file_tag);
    if (!requested.empty() && requested != "source") {
        description += fmt::format(" (EDEN_NX2_TRANSLATION_CACHE_KEY={} not recognised; use exe "
                                   "or source)",
                                   requested);
    }
    return {"source", std::move(value), std::move(file_tag), std::move(description)};
}

Digest MakeConfigDigest(const Shader::Profile& profile, const Shader::HostTranslateInfo& host_info,
                        u64 extra) {
    Hasher hasher;
    hasher.Add(FormatVersion);
    hasher.AddString(CurrentCacheIdentity().value);
#define TRANSLATION_CACHE_ADD(field) hasher.Add(profile.field);
    TRANSLATION_CACHE_PROFILE_FIELDS(TRANSLATION_CACHE_ADD)
#undef TRANSLATION_CACHE_ADD
    hasher.Add(host_info.min_ssbo_alignment);
    hasher.Add(host_info.max_per_stage_descriptor_sampled_images);
    hasher.Add(host_info.max_per_stage_resources);
    hasher.Add(host_info.max_descriptor_set_samplers);
    hasher.Add(host_info.max_descriptor_set_uniform_buffers);
    hasher.Add(host_info.max_descriptor_set_uniform_buffers_dynamic);
    hasher.Add(host_info.max_descriptor_set_storage_buffers);
    hasher.Add(host_info.max_descriptor_set_storage_buffers_dynamic);
    hasher.Add(host_info.max_descriptor_set_sampled_images);
    hasher.Add(host_info.max_descriptor_set_storage_images);
    hasher.Add(host_info.max_descriptor_set_input_attachements);
    hasher.Add(host_info.support_float64);
    hasher.Add(host_info.support_float16);
    hasher.Add(host_info.support_int64);
    hasher.Add(host_info.needs_demote_reorder);
    hasher.Add(host_info.support_snorm_render_buffer);
    hasher.Add(host_info.support_viewport_index_layer);
    hasher.Add(host_info.support_viewport_mask);
    hasher.Add(host_info.support_geometry_shader_passthrough);
    hasher.Add(host_info.support_conditional_barrier);
    hasher.Add(host_info.single_lane_geometry_subgroups);
    hasher.Add(host_info.support_byte_addressed_r16);
    // Settings and environment variables the recompiler reads directly.
    const auto& resolution = Settings::values.resolution_info;
    hasher.Add(resolution.up_scale);
    hasher.Add(resolution.down_shift);
    hasher.Add(resolution.up_factor);
    hasher.Add(resolution.down_factor);
    hasher.Add(resolution.active);
    hasher.Add(resolution.downscale);
    hasher.Add(Settings::values.disable_shader_loop_safety_checks.GetValue());
    hasher.Add(Settings::values.renderer_debug.GetValue());
    hasher.Add(Settings::values.fix_bloom_effects.GetValue());
    for (const char* variable : {"EDEN_NX2_UNDEF_ZERO", "EDEN_NX2_NO_TENSOR"}) {
        const char* value = std::getenv(variable);
        hasher.AddString(value ? value : "\x01unset");
    }
    hasher.Add(extra);
    return hasher.Finish();
}

Digest DigestRuntimeInfo(const Shader::RuntimeInfo& info) {
    Hasher hasher;
    hasher.Add(info.generic_input_types);
    hasher.Add(info.previous_stage_stores.mask);
    hasher.Add(info.per_primitive_generics.to_ullong());
    hasher.Add(static_cast<u64>(info.previous_stage_legacy_stores_mapping.size()));
    for (const auto& [from, to] : info.previous_stage_legacy_stores_mapping) {
        hasher.Add(from);
        hasher.Add(to);
    }
    hasher.Add(info.convert_depth_mode);
    hasher.Add(info.force_early_z);
    hasher.Add(info.tess_primitive);
    hasher.Add(info.tess_spacing);
    hasher.Add(info.tess_clockwise);
    hasher.Add(info.input_topology);
    hasher.Add(info.fixed_state_point_size.has_value());
    hasher.Add(info.fixed_state_point_size.value_or(0.0f));
    hasher.Add(info.alpha_test_func.has_value());
    hasher.Add(info.alpha_test_func.value_or(Shader::CompareFunction{}));
    hasher.Add(info.alpha_test_reference);
    hasher.Add(info.y_negate);
    hasher.Add(info.glasm_use_storage_buffers);
    hasher.Add(info.xfb_count);
    for (u32 i = 0; i < std::min<u32>(info.xfb_count, static_cast<u32>(info.xfb_varyings.size()));
         ++i) {
        hasher.Add(info.xfb_varyings[i]);
    }
    hasher.Add(info.color_output_types);
    hasher.Add(info.dual_source_blend);
    return hasher.Finish();
}

ProgramMeta ProgramMeta::From(const Shader::IR::Program& program) {
    return ProgramMeta{
        .stage = program.stage,
        .workgroup_size = program.workgroup_size,
        .output_topology = program.output_topology,
        .output_vertices = program.output_vertices,
        .invocations = program.invocations,
        .local_memory_size = program.local_memory_size,
        .shared_memory_size = program.shared_memory_size,
        .is_geometry_passthrough = program.is_geometry_passthrough,
        .info = program.info,
        .mesh = program.mesh,
    };
}

Shader::IR::Program ProgramMeta::ToProgram() const {
    Shader::IR::Program program;
    program.info = info;
    program.stage = stage;
    program.workgroup_size = workgroup_size;
    program.output_topology = output_topology;
    program.output_vertices = output_vertices;
    program.invocations = invocations;
    program.local_memory_size = local_memory_size;
    program.shared_memory_size = shared_memory_size;
    program.is_geometry_passthrough = is_geometry_passthrough;
    program.mesh = mesh;
    return program;
}

Digest MakeVariantKey(const Digest& entry_id, const Digest& runtime,
                      const Shader::Backend::Bindings& binding) {
    Hasher hasher;
    hasher.Add(entry_id.lo);
    hasher.Add(entry_id.hi);
    hasher.Add(runtime.lo);
    hasher.Add(runtime.hi);
    hasher.Add(binding.unified);
    hasher.Add(binding.uniform_buffer);
    hasher.Add(binding.storage_buffer);
    hasher.Add(binding.texture);
    hasher.Add(binding.image);
    hasher.Add(binding.texture_scaling_index);
    hasher.Add(binding.image_scaling_index);
    return hasher.Finish();
}

void WriteInfo(std::vector<u8>& out, const Shader::Info& info) {
    Writer writer{out};
#define TRANSLATION_CACHE_POD(field) writer.Put(info.field);
#define TRANSLATION_CACHE_MAP(field)                                                               \
    writer.Put(static_cast<u32>(info.field.size()));                                               \
    for (const auto& [key, value] : info.field) {                                                  \
        writer.Put(key);                                                                           \
        writer.Put(value);                                                                         \
    }
#define TRANSLATION_CACHE_VEC(field) writer.PutVector(info.field);
#define TRANSLATION_CACHE_VECVEC(field)                                                            \
    writer.Put(static_cast<u32>(info.field.size()));                                               \
    for (const auto& inner : info.field) {                                                         \
        writer.PutVector(inner);                                                                   \
    }
    TRANSLATION_CACHE_INFO_FIELDS(TRANSLATION_CACHE_POD, TRANSLATION_CACHE_MAP,
                                  TRANSLATION_CACHE_VEC, TRANSLATION_CACHE_VECVEC)
#undef TRANSLATION_CACHE_POD
#undef TRANSLATION_CACHE_MAP
#undef TRANSLATION_CACHE_VEC
#undef TRANSLATION_CACHE_VECVEC
}

bool ReadInfo(std::span<const u8>& in, Shader::Info& info) {
    Reader reader{in};
    info = {};
#define TRANSLATION_CACHE_POD(field)                                                               \
    if (!reader.Get(info.field)) {                                                                 \
        return false;                                                                              \
    }
#define TRANSLATION_CACHE_MAP(field)                                                               \
    {                                                                                              \
        u32 size{};                                                                                \
        if (!reader.Get(size) || size > MaxElements) {                                             \
            return false;                                                                          \
        }                                                                                          \
        for (u32 i = 0; i < size; ++i) {                                                           \
            typename decltype(info.field)::key_type key{};                                         \
            typename decltype(info.field)::mapped_type value{};                                    \
            if (!reader.Get(key) || !reader.Get(value)) {                                          \
                return false;                                                                      \
            }                                                                                      \
            info.field.emplace(key, value);                                                        \
        }                                                                                          \
    }
#define TRANSLATION_CACHE_VEC(field)                                                               \
    if (!reader.GetVector(info.field, CapacityOf(info.field))) {                                   \
        return false;                                                                              \
    }
#define TRANSLATION_CACHE_VECVEC(field)                                                            \
    {                                                                                              \
        u32 size{};                                                                                \
        if (!reader.Get(size) || size > MaxElements) {                                             \
            return false;                                                                          \
        }                                                                                          \
        info.field.resize(size);                                                                   \
        for (auto& inner : info.field) {                                                           \
            if (!reader.GetVector(inner, MaxElements)) {                                           \
                return false;                                                                      \
            }                                                                                      \
        }                                                                                          \
    }
    TRANSLATION_CACHE_INFO_FIELDS(TRANSLATION_CACHE_POD, TRANSLATION_CACHE_MAP,
                                  TRANSLATION_CACHE_VEC, TRANSLATION_CACHE_VECVEC)
#undef TRANSLATION_CACHE_POD
#undef TRANSLATION_CACHE_MAP
#undef TRANSLATION_CACHE_VEC
#undef TRANSLATION_CACHE_VECVEC
    return true;
}

bool InfoEqual(const Shader::Info& lhs, const Shader::Info& rhs) {
    const auto pod_equal = [](const auto& a, const auto& b) {
        if constexpr (requires { a.mask; }) {
            return a.mask == b.mask;
        } else {
            return a == b;
        }
    };
#define TRANSLATION_CACHE_EQ_POD(field)                                                            \
    if (!pod_equal(lhs.field, rhs.field)) {                                                        \
        return false;                                                                              \
    }
#define TRANSLATION_CACHE_EQ_RANGE(field)                                                          \
    if (!std::ranges::equal(lhs.field, rhs.field)) {                                               \
        return false;                                                                              \
    }
    TRANSLATION_CACHE_INFO_FIELDS(TRANSLATION_CACHE_EQ_POD, TRANSLATION_CACHE_EQ_RANGE,
                                  TRANSLATION_CACHE_EQ_RANGE, TRANSLATION_CACHE_EQ_RANGE)
#undef TRANSLATION_CACHE_EQ_POD
#undef TRANSLATION_CACHE_EQ_RANGE
    return true;
}

bool LayoutIsCovered() {
    return CountFields<Shader::Info>() == InfoFields &&
           CountFields<Shader::Profile>() == ProfileFields &&
           CountFields<Shader::HostTranslateInfo>() == HostFields &&
           CountFields<Shader::RuntimeInfo>() == RuntimeInfoFields &&
           CountFields<Shader::IR::Program>() == ProgramFields &&
           CountFields<Shader::Backend::Bindings>() == BindingsFields;
}

Store::~Store() {
    Close();
}

void Store::Close() {
    std::scoped_lock lock{mutex};
    if (file.is_open()) {
        file.flush();
        file.close();
    }
    entries.clear();
    variants.clear();
    enabled = false;
    ++generation;
}

void Store::Open(const std::filesystem::path& path, const Digest& config) {
    Close();
    if (!LayoutIsCovered()) {
        LOG_WARNING(Render_Vulkan,
                    "Translation cache off: a shader structure has fields it does not store "
                    "(update shader_translation_cache.cpp)");
        return;
    }
    std::scoped_lock lock{mutex};
    enabled = true;
    if (path.empty()) {
        return;
    }
    const CacheIdentity identity = CurrentCacheIdentity();
    if (identity.value.empty()) {
        LOG_WARNING(Render_Vulkan,
                    "Translation cache kept in memory only: the build cannot be identified ({})",
                    identity.description);
        return;
    }
    LOG_INFO(Render_Vulkan, "Translation cache: translator identity {}", identity.description);
    filename = path;
    const auto begin = std::chrono::steady_clock::now();
    const bool loaded = LoadFile(path, config);
    if (!loaded) {
        file.open(path, std::ios::binary | std::ios::trunc);
        file.write(FileMagic.data(), FileMagic.size());
        file.write(reinterpret_cast<const char*>(&FormatVersion), sizeof(FormatVersion));
        file.write(reinterpret_cast<const char*>(&config), sizeof(config));
        file.flush();
    }
    LOG_INFO(Render_Vulkan,
             "Translation cache: {} stage entries and {} modules read from {} in {:.1f} ms",
             loaded_entries, loaded_variants, Common::FS::PathToUTF8String(path.filename()),
             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin)
                 .count());
}

bool Store::LoadFile(const std::filesystem::path& path, const Digest& config) {
    loaded_entries = 0;
    loaded_variants = 0;
    std::ifstream in{path, std::ios::binary | std::ios::ate};
    if (!in.is_open()) {
        return false;
    }
    const auto size = static_cast<size_t>(in.tellg());
    std::vector<u8> data(size);
    in.seekg(0);
    in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
    in.close();
    constexpr size_t HeaderSize = FileMagic.size() + sizeof(u32) + sizeof(Digest);
    if (size < HeaderSize || std::memcmp(data.data(), FileMagic.data(), FileMagic.size()) != 0) {
        return false;
    }
    u32 version{};
    Digest file_config{};
    std::memcpy(&version, data.data() + FileMagic.size(), sizeof(version));
    std::memcpy(&file_config, data.data() + FileMagic.size() + sizeof(version), sizeof(Digest));
    if (version != FormatVersion || file_config != config) {
        LOG_INFO(Render_Vulkan,
                 "Translation cache: written by another build or configuration, starting over");
        return false;
    }
    size_t offset = HeaderSize;
    while (offset + 8 <= size) {
        u32 magic{}, length{};
        std::memcpy(&magic, data.data() + offset, sizeof(magic));
        std::memcpy(&length, data.data() + offset + 4, sizeof(length));
        if (magic != RecordMagic || offset + 8 + length > size) {
            break;
        }
        std::span<const u8> record{data.data() + offset + 8, length};
        Reader reader{record};
        u8 type{};
        bool ok = reader.Get(type);
        if (ok && type == RecordEntry) {
            Entry entry;
            ok = reader.Get(entry.stage_key) && ReadQueries(reader, entry.queries) &&
                 ReadMeta(record, entry.meta) && record.empty();
            if (ok) {
                InsertLocked(std::move(entry));
                ++loaded_entries;
            }
        } else if (ok && type == RecordVariant) {
            Digest key{};
            auto stored = std::make_shared<StoredVariant>();
            ok = reader.Get(key) && reader.Get(stored->binding_out) &&
                 ReadInfo(record, stored->info) && reader.Get(stored->spirv_words) &&
                 reader.GetBytes(stored->compressed_spirv) && record.empty();
            if (ok) {
                variants.insert_or_assign(key, std::move(stored));
                ++loaded_variants;
            }
        } else {
            ok = false;
        }
        if (!ok) {
            break;
        }
        offset += 8 + length;
    }
    if (offset != size) {
        // A record cut short (the emulator stopped while writing) or damaged: keep what precedes.
        LOG_WARNING(Render_Vulkan, "Translation cache: dropping {} bytes after offset {}",
                    size - offset, offset);
        std::error_code ec;
        std::filesystem::resize_file(path, offset, ec);
        if (ec) {
            return false;
        }
    }
    file.open(path, std::ios::binary | std::ios::app);
    return file.is_open();
}

std::shared_ptr<const Entry> Store::InsertLocked(Entry entry) {
    entry.id = EntryId(entry.stage_key, entry.queries);
    auto& list = entries[entry.stage_key];
    for (const auto& existing : list) {
        if (existing->id == entry.id && existing->queries == entry.queries) {
            return existing;
        }
    }
    auto shared = std::make_shared<const Entry>(std::move(entry));
    list.push_back(shared);
    return shared;
}

std::shared_ptr<const Entry> Store::FindEntry(const Digest& stage_key, Shader::Environment& env,
                                              LiveAnswers* live) {
    std::vector<std::shared_ptr<const Entry>> candidates;
    {
        std::scoped_lock lock{mutex};
        const auto it = entries.find(stage_key);
        if (it == entries.end()) {
            return nullptr;
        }
        candidates = it->second;
    }
    size_t furthest = 0;
    std::optional<QueryKind> furthest_kind;
    for (const auto& candidate : candidates) {
        try {
            const size_t matched = ReplayQueriesUntilMismatch(candidate->queries, env, live);
            if (matched == candidate->queries.size()) {
                return candidate;
            }
            if (!furthest_kind || matched > furthest) {
                furthest = matched;
                furthest_kind = candidate->queries[matched].kind;
            }
        } catch (const Shader::Exception&) {
            // An environment read from the pipeline cache only answers the queries its own
            // translation made; a question outside them means this entry does not apply.
        }
    }
    if (count_mismatches && furthest_kind) {
        ++first_mismatch[static_cast<size_t>(*furthest_kind) % first_mismatch.size()];
    }
    return nullptr;
}

bool Store::HasStageKey(const Digest& stage_key) {
    std::scoped_lock lock{mutex};
    return entries.contains(stage_key);
}

std::optional<Variant> Store::FindVariant(const Digest& variant_key) {
    std::shared_ptr<const StoredVariant> stored;
    {
        std::scoped_lock lock{mutex};
        const auto it = variants.find(variant_key);
        if (it == variants.end()) {
            return std::nullopt;
        }
        stored = it->second;
    }
    Variant variant{
        .binding_out = stored->binding_out,
        .info = stored->info,
        .spirv = std::vector<u32>(stored->spirv_words),
    };
    const size_t bytes = Common::Compression::DecompressDataZSTD(
        stored->compressed_spirv,
        std::span<u8>{reinterpret_cast<u8*>(variant.spirv.data()),
                      variant.spirv.size() * sizeof(u32)});
    if (bytes != variant.spirv.size() * sizeof(u32)) {
        return std::nullopt;
    }
    return variant;
}

std::shared_ptr<const Entry> Store::Insert(const Digest& stage_key, std::vector<Query> queries,
                                           const ProgramMeta& meta,
                                           std::optional<u64> expected_generation) {
    Entry entry{.stage_key = stage_key, .queries = std::move(queries), .meta = meta};
    std::vector<u8> record;
    {
        Writer writer{record};
        writer.Put(RecordEntry);
        writer.Put(entry.stage_key);
        WriteQueries(writer, entry.queries);
        WriteMeta(record, entry.meta);
    }
    std::scoped_lock lock{mutex};
    if (!enabled || (expected_generation && *expected_generation != generation.load())) {
        return nullptr;
    }
    const size_t before = entries.contains(stage_key) ? entries[stage_key].size() : 0;
    auto shared = InsertLocked(std::move(entry));
    if (entries[stage_key].size() != before) {
        Append(record);
    }
    return shared;
}

bool Store::InsertVariant(const Entry& entry, const Digest& variant_key, const Variant& variant) {
    auto stored = std::make_shared<StoredVariant>();
    stored->binding_out = variant.binding_out;
    stored->info = variant.info;
    stored->spirv_words = static_cast<u32>(variant.spirv.size());
    stored->compressed_spirv = Common::Compression::CompressDataZSTD(
        reinterpret_cast<const u8*>(variant.spirv.data()), variant.spirv.size() * sizeof(u32), 3);
    std::vector<u8> record;
    {
        Writer writer{record};
        writer.Put(RecordVariant);
        writer.Put(variant_key);
        writer.Put(stored->binding_out);
        WriteInfo(record, stored->info);
        writer.Put(stored->spirv_words);
        writer.PutBytes(stored->compressed_spirv.data(), stored->compressed_spirv.size());
    }
    std::scoped_lock lock{mutex};
    if (!enabled) {
        return false;
    }
    // Only for an entry of this store: one held across a reopen (or never inserted) names a
    // program of another configuration.
    const auto list = entries.find(entry.stage_key);
    if (list == entries.end() ||
        std::ranges::none_of(list->second, [&](const auto& stored_entry) {
            return stored_entry.get() == &entry;
        })) {
        ++rejected_variants;
        return false;
    }
    if (variants.try_emplace(variant_key, std::move(stored)).second) {
        Append(record);
    }
    return true;
}

void Store::Append(const std::vector<u8>& record) {
    if (!file.is_open()) {
        return;
    }
    const u32 length = static_cast<u32>(record.size());
    file.write(reinterpret_cast<const char*>(&RecordMagic), sizeof(RecordMagic));
    file.write(reinterpret_cast<const char*>(&length), sizeof(length));
    file.write(reinterpret_cast<const char*>(record.data()), length);
    file.flush();
    if (!file.good()) {
        LOG_ERROR(Render_Vulkan, "Translation cache: writing {} failed, keeping it in memory only",
                  Common::FS::PathToUTF8String(filename));
        file.close();
    }
}

void Store::LogStatistics(std::string_view when) {
    const u64 hits = stage_hits.load();
    const u64 misses = stage_misses.load();
    if (hits + misses == 0) {
        return;
    }
    const double hit_ms = static_cast<double>(hit_ns.load()) / 1e6;
    const double miss_ms = static_cast<double>(miss_ns.load()) / 1e6;
    const double miss_avg = misses ? miss_ms / static_cast<double>(misses) : 0.0;
    const double hit_avg = hits ? hit_ms / static_cast<double>(hits) : 0.0;
    LOG_INFO(Render_Vulkan,
             "Translation cache ({}): {} stages from the cache in {:.1f} ms ({:.3f} ms each), {} "
             "translated in {:.1f} ms ({:.3f} ms each){}",
             when, hits, hit_ms, hit_avg, misses, miss_ms, miss_avg,
             misses && hits ? fmt::format("; about {:.0f} ms of translation saved",
                                          static_cast<double>(hits) * (miss_avg - hit_avg))
                            : std::string{});
}

} // namespace VideoCommon::TranslationCache
