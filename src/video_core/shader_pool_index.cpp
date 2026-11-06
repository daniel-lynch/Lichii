// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <span>

#include "common/logging.h"
#include "video_core/memory_manager.h"
#include "video_core/shader_pool_index.h"

namespace VideoCommon {
namespace {
constexpr std::array<char, 8> Magic{'L', 'X', 'A', 'O', 'T', 'I', 'X', '2'};
constexpr size_t ProgramRecordSize = 4 + 4 + 1 + 8 + 4 + 2;
/// A pool counts as recognised when this share of its programs verifies at the expected address.
constexpr double RecogniseShare = 0.9;
} // Anonymous namespace

u64 ShaderPoolIndex::Fingerprint(std::span<const u8> bytes) {
    u64 hash = 0xcbf29ce484222325ULL;
    for (const u8 byte : bytes) {
        hash = (hash ^ byte) * 0x100000001b3ULL;
    }
    return hash;
}

bool ShaderPoolIndex::Parse(std::span<const u8> data, std::vector<Container>& out) {
    out.clear();
    if (data.size() < Magic.size() + 4 || std::memcmp(data.data(), Magic.data(), Magic.size())) {
        return false;
    }
    size_t offset = Magic.size();
    const auto get = [&](auto& value) {
        if (offset + sizeof(value) > data.size()) {
            return false;
        }
        std::memcpy(&value, data.data() + offset, sizeof(value));
        offset += sizeof(value);
        return true;
    };
    u32 count{};
    if (!get(count) || count > (data.size() - offset) / 4) {
        return false;
    }
    out.resize(count);
    for (Container& container : out) {
        u32 programs{};
        if (!get(programs) || programs > (data.size() - offset) / ProgramRecordSize) {
            return false;
        }
        container.programs.resize(programs);
        for (Program& program : container.programs) {
            if (!get(program.offset) || !get(program.size) || !get(program.stage) ||
                !get(program.fingerprint) || !get(program.constants_offset) ||
                !get(program.constants_size) || program.constants_size > 0x100) {
                return false;
            }
        }
    }
    return offset == data.size();
}

std::unique_ptr<ShaderPoolIndex> ShaderPoolIndex::Open() {
    const char* path = std::getenv("EDEN_NX2_AOT_INDEX");
    if (path == nullptr || path[0] == '\0') {
        return nullptr;
    }
    std::ifstream file{std::filesystem::path{path}, std::ios::binary | std::ios::ate};
    if (!file.is_open()) {
        LOG_WARNING(HW_GPU, "AOT pool index: cannot open {}", path);
        return nullptr;
    }
    std::vector<u8> data(static_cast<size_t>(file.tellg()));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    std::vector<Container> containers;
    if (!file || !Parse(data, containers)) {
        LOG_WARNING(HW_GPU, "AOT pool index: {} is not a valid index", path);
        return nullptr;
    }
    return std::unique_ptr<ShaderPoolIndex>(new ShaderPoolIndex(std::move(containers)));
}

ShaderPoolIndex::ShaderPoolIndex(std::vector<Container> containers_)
    : containers{std::move(containers_)} {
    size_t programs = 0;
    for (u32 c = 0; c < containers.size(); ++c) {
        const auto& list = containers[c].programs;
        programs += list.size();
        for (u32 p = 0; p < list.size(); ++p) {
            by_fingerprint.emplace(list[p].fingerprint, std::pair{c, p});
            if (list[p].size < FingerprintBytes) {
                short_sizes.push_back(list[p].size);
            }
        }
    }
    std::ranges::sort(short_sizes);
    short_sizes.erase(std::unique(short_sizes.begin(), short_sizes.end()), short_sizes.end());
    LOG_INFO(HW_GPU, "AOT pool index: {} containers, {} programs", containers.size(), programs);
}

ShaderPoolIndex::~ShaderPoolIndex() {
    LogStatistics("end");
}

void ShaderPoolIndex::LogStatistics(const char* when) const {
    LOG_INFO(HW_GPU,
             "AOT pool index ({}): {} first uses, {} found in the index, {} predicted before first "
             "use; {} ambiguous; pools {} recognised, {} rejected; siblings {} verified, {} "
             "differed, {} unmapped; {} still predicted unused; candidates {} offered, {} "
             "dropped, {} queued; recognition {:.1f} ms, candidate environments {:.1f} ms on the "
             "GPU thread",
             when, first_uses, first_uses_indexed, first_uses_predicted, ambiguous,
             containers_recognised, containers_rejected, siblings_verified, siblings_mismatched,
             siblings_unmapped, predicted.size(), candidates_offered, candidates_dropped,
             candidates_queue.size(), std::chrono::duration<double, std::milli>(time).count(),
             std::chrono::duration<double, std::milli>(seed_time).count());
}

std::vector<ShaderPoolIndex::Candidate> ShaderPoolIndex::TakeCandidates(size_t count) {
    std::vector<Candidate> result;
    while (result.size() < count && !candidates_queue.empty()) {
        const Candidate candidate = candidates_queue.front();
        candidates_queue.pop_front();
        if (!used.contains(candidate.address)) {
            result.push_back(candidate);
        }
    }
    return result;
}

void ShaderPoolIndex::OnFirstUse(Tegra::MemoryManager& gpu_memory, GPUVAddr address) {
    const auto begin = std::chrono::steady_clock::now();
    ++first_uses;
    if (predicted.erase(address) != 0) {
        ++first_uses_predicted;
    }
    std::array<u8, FingerprintBytes> bytes{};
    const auto read = [&](GPUVAddr where, size_t size) {
        if (!gpu_memory.IsFullyMappedRange(where, size)) {
            return false;
        }
        gpu_memory.ReadBlockUnsafe(where, bytes.data(), size);
        return true;
    };
    used.insert(address);
    // The program's own fingerprint, at every length an indexed program's fingerprint covers.
    std::vector<std::pair<u32, u32>> candidates;
    if (read(address, FingerprintBytes)) {
        u64 hash = Fingerprint({});
        auto next_short = short_sizes.begin();
        for (size_t length = 1; length <= FingerprintBytes; ++length) {
            hash = (hash ^ bytes[length - 1]) * 0x100000001b3ULL;
            const bool is_short = next_short != short_sizes.end() && *next_short == length;
            if (!is_short && length != FingerprintBytes) {
                continue;
            }
            if (is_short) {
                ++next_short;
            }
            const auto [first, last] = by_fingerprint.equal_range(hash);
            for (auto it = first; it != last; ++it) {
                const u32 size = containers[it->second.first].programs[it->second.second].size;
                if (length < FingerprintBytes ? size == length : size >= FingerprintBytes) {
                    candidates.push_back(it->second);
                }
            }
        }
    }
    if (!candidates.empty()) {
        ++first_uses_indexed;
    }
    if (candidates.size() > MaxCandidates) {
        ++ambiguous;
        candidates.clear();
    }
    for (const auto [container_index, program_index] : candidates) {
        const Container& container = containers[container_index];
        const GPUVAddr pool = address - container.programs[program_index].offset;
        // A decided (container, pool) pair is not checked again. A pair that failed only because
        // part of the pool was not mapped yet stays undecided and is checked on a later use.
        const u64 pair_key = (u64{container_index} << 40) ^ pool;
        if (checked.contains(pair_key)) {
            continue;
        }
        bool sample_unmapped = false;
        const auto matches = [&](const Program& program) {
            const size_t length = (std::min)(size_t{program.size}, FingerprintBytes);
            if (!read(pool + program.offset, length)) {
                sample_unmapped = true;
                return false;
            }
            return Fingerprint(std::span{bytes.data(), length}) == program.fingerprint;
        };
        // A container the program only shares (a common effect program) fails at once: check a
        // few spread-out programs before reading all of them.
        const size_t total_programs = container.programs.size();
        bool sample_ok = true;
        for (size_t probe = 1; probe <= 8 && sample_ok; ++probe) {
            sample_ok = matches(container.programs[(total_programs - 1) * probe / 8]);
        }
        if (!sample_ok) {
            ++containers_rejected;
            if (!sample_unmapped) {
                checked.insert(pair_key);
            }
            continue;
        }
        size_t verified = 0, mismatched = 0, unmapped = 0;
        std::vector<std::pair<GPUVAddr, const Program*>> found;
        for (const Program& program : container.programs) {
            const GPUVAddr where = pool + program.offset;
            const size_t length = (std::min)(size_t{program.size}, FingerprintBytes);
            if (!read(where, length)) {
                ++unmapped;
            } else if (Fingerprint(std::span{bytes.data(), length}) != program.fingerprint) {
                ++mismatched;
            } else {
                ++verified;
                found.emplace_back(where, &program);
            }
        }
        const size_t total = container.programs.size();
        if (unmapped == 0 ||
            static_cast<double>(verified) >= RecogniseShare * static_cast<double>(total)) {
            checked.insert(pair_key);
        }
        if (static_cast<double>(verified) >= RecogniseShare * static_cast<double>(total)) {
            ++containers_recognised;
            siblings_verified += verified;
            siblings_mismatched += mismatched;
            siblings_unmapped += unmapped;
            for (const auto& [where, program] : found) {
                const u8 stage = program->stage;
                if (used.contains(where)) {
                    continue;
                }
                predicted.insert(where);
                // Vertex and pixel programs only: their container slot maps to one pipeline slot.
                if (stage != 0 && stage != 4) {
                    continue;
                }
                if (candidates_queue.size() < MaxCandidatesQueued) {
                    candidates_queue.push_back(
                        {where, stage, program->constants_offset, program->constants_size});
                    ++candidates_offered;
                } else {
                    ++candidates_dropped;
                }
            }
        } else {
            ++containers_rejected;
        }
    }
    time += std::chrono::steady_clock::now() - begin;
    if (first_uses % 100 == 0) {
        LogStatistics("so far");
    }
}

} // namespace VideoCommon
