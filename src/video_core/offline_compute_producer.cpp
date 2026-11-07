// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <span>
#include <stdexcept>
#include "common/logging.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/frontend/maxwell/structured_control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "video_core/offline_compute_producer.h"
#include "video_core/shader_archive_environment.h"

namespace VideoCommon::OfflineShaders {
namespace {
// The runtime's compute stage-key slot (PipelineCache::CreateComputePipeline).
constexpr u32 ComputeSlot = 5;
// Compute controls are 0xe0 or 0xf8 bytes; the dispatch state ends at 0x6c.
constexpr u64 ComputeControlBytes = 0x6c;

std::vector<u8> ReadRange(const std::filesystem::path& path, u64 offset, u64 size) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file || file.tellg() < 0) throw std::runtime_error("input_io");
    const auto length = static_cast<u64>(file.tellg());
    if (offset > length || size > length - offset || size > MaxProgramBytes)
        throw std::runtime_error("invalid_range");
    std::vector<u8> bytes(size);
    file.seekg(static_cast<std::streamoff>(offset));
    if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)))
        throw std::runtime_error("input_io");
    return bytes;
}

u32 LoadWord(std::span<const u8> bytes, size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(u32))
        throw std::runtime_error("invalid_range");
    return u32{bytes[offset]} | (u32{bytes[offset + 1]} << 8) |
           (u32{bytes[offset + 2]} << 16) | (u32{bytes[offset + 3]} << 24);
}

// Local memory loads and stores without a local memory size would make the emitter index a
// variable it never declared (an abort inside sirit, not an exception). The runtime would fail
// the same way, so such a program has no runtime key worth producing.
bool UsesLocalMemory(const Shader::IR::Program& program) {
    for (const Shader::IR::Block* block : program.post_order_blocks) {
        for (const Shader::IR::Inst& inst : block->Instructions()) {
            switch (inst.GetOpcode()) {
            case Shader::IR::Opcode::LoadLocal:
            case Shader::IR::Opcode::WriteLocal:
                return true;
            default:
                break;
            }
        }
    }
    return false;
}

// Duplicate rows of one program serialize through lookup and append.
std::array<std::mutex, 64> compute_locks;
} // namespace

bool ComputeEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_OFFLINE_COMPUTE");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

ComputeOutcome ProduceCompute(const Program& row, const Shader::Profile& profile,
                              const Shader::HostTranslateInfo& host,
                              const TranslationCache::Digest& config,
                              TranslationCache::Store& store, u32 shared_memory_limit,
                              const TranslationCache::RecipeCatalog* recipes) {
    namespace TC = TranslationCache;
    if (row.stage != Shader::Stage::Compute) throw std::runtime_error("unsupported_stage");
    if (!row.skip_reason.empty()) throw std::runtime_error(row.skip_reason);
    const auto program_bytes = ReadRange(row.container, row.offset, row.size);
    const auto control = ReadRange(row.control, row.control_offset, ComputeControlBytes);
    u64 fingerprint = 0xcbf29ce484222325ULL;
    for (const auto byte : std::span(program_bytes).first(std::min<size_t>(256, row.size)))
        fingerprint = (fingerprint ^ byte) * 0x100000001b3ULL;
    if (fingerprint != row.fingerprint) throw std::runtime_error("fingerprint_mismatch");
    if (LoadWord(control, 0x40) != row.constants_offset ||
        LoadWord(control, 0x3c) != row.constants_size)
        throw std::runtime_error("constants_mismatch");
    for (size_t i = 0; i < row.bank1_words.size(); ++i)
        if (LoadWord(program_bytes, row.constants_offset + i * 4) != row.bank1_words[i])
            throw std::runtime_error("constants_mismatch");

    // Unrecorded bindings use a replay-guarded fallback.
    const auto texture_type = TextureTypeForProgram(row);
    ArchiveEnvironment env(ArchiveEnvironment::ComputeProgram{}, program_bytes, control, 0,
                           texture_type);
    const auto hash = env.Analyze();
    if (!hash) throw std::runtime_error("unsupported_lowering");
    const auto key = TC::MakeStageKey(env, *hash, ComputeSlot, config);
    std::scoped_lock lock(compute_locks[key.lo % compute_locks.size()]);

    ComputeOutcome outcome;
    // One entry per recorded answer set: the guess (every direct slot mapped, no
    // indirect one), then each set recorded at run time for this program. The runtime replays
    // the answers against the live constant buffers, so an entry is served only where the
    // same slots hold mapped pointers; elsewhere it falls back to translating.
    std::vector<std::span<const TC::Query>> answer_sets{{}};
    if (recipes != nullptr) {
        const auto source_key = TC::MakeStageKey(env, *hash, ComputeSlot, recipes->source_config);
        if (const auto it = recipes->recorded_answers.find(source_key);
            it != recipes->recorded_answers.end()) {
            for (const auto& set : it->second) answer_sets.emplace_back(set);
        }
    }
    for (size_t set = 0; set < answer_sets.size(); ++set) {
        try {
            ArchiveEnvironment variant_env(ArchiveEnvironment::ComputeProgram{}, program_bytes,
                                           control, 0, texture_type);
            if (set != 0) variant_env.SetRecordedAnswers(answer_sets[set]);
            if (set != 0) ++outcome.recorded_sets;
            TC::RecordingEnvironment resume(variant_env, true);
            auto entry = store.FindEntry(key, resume);
            bool invalidated = false;
            bool ready = false;
            if (entry) {
                const auto state = store.ValidateVariantForResume(
                    TC::MakeVariantKey(entry->id, TC::Digest{}, Shader::Backend::Bindings{}));
                outcome.validated |= state == TC::Store::ResumeVariant::Validated;
                invalidated = state == TC::Store::ResumeVariant::Invalidated;
                outcome.invalidated |= invalidated;
                if (state == TC::Store::ResumeVariant::Ready ||
                    state == TC::Store::ResumeVariant::Validated) {
                    // Already produced, or an earlier set's entry answers this set the same way.
                    if (set == 0) outcome.resumed = true;
                    ready = true;
                    if (set == 0) continue;
                }
            }
            // As the runtime translates it: the control flow starts at the environment's start
            // address (past the zeroed header), not past a graphics program header.
            TC::RecordingEnvironment recording(variant_env, true);
            Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_pool;
            Shader::ObjectPool<Shader::IR::Inst> inst_pool;
            Shader::ObjectPool<Shader::IR::Block> block_pool;
            Shader::Maxwell::Flow::CFG cfg(recording, flow_pool,
                                           Shader::Maxwell::Location{variant_env.StartAddress()});
            auto program =
                Shader::Maxwell::TranslateProgram(inst_pool, block_pool, recording, cfg, host);
            if (!variant_env.CanCacheTranslation())
                throw std::runtime_error("lowering_or_emission_rejected");
            if (program.local_memory_size == 0 && UsesLocalMemory(program))
                throw std::runtime_error("unsupported_local_memory");
            outcome.translated = true;
            if (!entry || set != 0) {
                const auto fresh = store.Insert(key, recording.TakeQueries(), TC::ProgramMeta::From(program), true);
                if (!fresh) throw std::runtime_error("cache_disabled");
                if (!entry || entry->id != fresh->id) {
                    // The first replay-compatible entry need not be this transcript. Resume
                    // the exact entry selected by current translation, not its predecessor.
                    const auto state = store.ValidateVariantForResume(
                        TC::MakeVariantKey(fresh->id, TC::Digest{}, Shader::Backend::Bindings{}));
                    outcome.validated |= state == TC::Store::ResumeVariant::Validated;
                    invalidated = state == TC::Store::ResumeVariant::Invalidated;
                    outcome.invalidated |= invalidated;
                    ready = state == TC::Store::ResumeVariant::Ready ||
                            state == TC::Store::ResumeVariant::Validated;
                }
                entry = fresh;
            }
            if (set != 0) {
                if (!TC::ReplayQueries(entry->queries, variant_env))
                    throw std::runtime_error("recorded_replay_failed");
                ++outcome.recorded_validated;
            }
            if (ready) continue;
            // The runtime records the entry before clamping and emits after it.
            if (shared_memory_limit != 0 && program.shared_memory_size > shared_memory_limit)
                program.shared_memory_size = shared_memory_limit;
            const auto code = Shader::Backend::SPIRV::EmitSPIRV(profile, program);
            if (code.size() < 5 || code.front() != 0x07230203)
                throw std::runtime_error("lowering_or_emission_rejected");
            if (!store.InsertVariant(
                    *entry, TC::MakeVariantKey(entry->id, TC::Digest{}, Shader::Backend::Bindings{}),
                    {{}, program.info, code, true}))
                throw std::runtime_error("cache_disabled");
            ++outcome.modules;
            outcome.replaced += invalidated ? 1 : 0;
            outcome.pointer_modules += set != 0 ? 1 : 0;
            outcome.module_bytes += code.size() * sizeof(u32);
        } catch (const std::exception& error) {
            if (set == 0 && answer_sets.size() == 1) throw;
            if (set != 0) ++outcome.recorded_failed;
            LOG_WARNING(Render_Vulkan, "Offline compute recorded answers: key={:016x}{:016x} set={}: {}",
                        key.hi, key.lo, set, error.what());
        }
    }
    return outcome;
}
} // namespace VideoCommon::OfflineShaders
