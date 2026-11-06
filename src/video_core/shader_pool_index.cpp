// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <span>
#include <utility>
#include <string>

#include <nlohmann/json.hpp>

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

bool ShaderPoolIndex::ParseManifest(std::string_view text, std::vector<ManifestRow>& out) {
    out.clear();
    const auto json = nlohmann::json::parse(text, nullptr, false);
    if (json.is_discarded() || !json.is_object() || !json.contains("programs") ||
        !json["programs"].is_array()) {
        return false;
    }
    // Container stage slots, as tools/oot/container/bnsh.py STAGES numbers them.
    static constexpr std::array<std::pair<std::string_view, u8>, 10> stages{{
        {"vertex", 0},
        {"hull", 1},
        {"domain", 2},
        {"geometry", 3},
        {"fragment", 4},
        {"pixel", 4},
        {"compute", 5},
        {"task", 7},
        {"mesh", 8},
        {"slot6", 6},
    }};
    for (const auto& program : json["programs"]) {
        if (!program.is_object()) {
            return false;
        }
        const auto fingerprint = program.find("fingerprint");
        const auto pool_offset = program.find("pool_offset");
        const auto size = program.find("size");
        const auto stage = program.find("stage");
        const auto tier = program.find("tier");
        if (fingerprint == program.end() || !fingerprint->is_string() ||
            pool_offset == program.end() || !pool_offset->is_number_unsigned() ||
            size == program.end() || !size->is_number_unsigned() || stage == program.end() ||
            !stage->is_string() || tier == program.end() || !tier->is_number_unsigned()) {
            return false;
        }
        const std::string fingerprint_text = fingerprint->get<std::string>();
        if (fingerprint_text.empty() || fingerprint_text.size() > 16) {
            return false;
        }
        ManifestRow row{};
        try {
            size_t parsed = 0;
            row.fingerprint = std::stoull(fingerprint_text, &parsed, 16);
            if (parsed != fingerprint_text.size()) {
                return false;
            }
        } catch (const std::exception&) {
            return false;
        }
        row.pool_offset = pool_offset->get<u32>();
        row.size = size->get<u32>();
        const u64 tier_value = tier->get<u64>();
        // Tier 4 (dynamic passes) is off: those programs are siblings like any other.
        if (tier_value < 1 || tier_value >= PriorityCount) {
            continue;
        }
        row.tier = static_cast<u8>(tier_value);
        const std::string stage_name = stage->get<std::string>();
        const auto slot = std::ranges::find(stages, std::string_view{stage_name},
                                            &std::pair<std::string_view, u8>::first);
        if (slot == stages.end()) {
            continue;
        }
        row.stage = slot->second;
        if (const auto bank1 = program.find("bank1"); bank1 != program.end() && bank1->is_object()) {
            const auto offset = bank1->find("offset");
            const auto words = bank1->find("words");
            if (offset != bank1->end() && offset->is_number_unsigned() && words != bank1->end() &&
                words->is_array() && words->size() <= 0x40) {
                row.bank1_offset = offset->get<u32>();
                for (const auto& word : *words) {
                    if (!word.is_number_unsigned()) {
                        return false;
                    }
                    row.bank1_words.push_back(word.get<u32>());
                }
            }
        }
        out.push_back(std::move(row));
    }
    return true;
}

size_t ShaderPoolIndex::ApplyManifest(std::span<const ManifestRow> rows) {
    // (fingerprint, pool offset) -> programs with that key.
    std::unordered_multimap<u64, Program*> by_key;
    for (Container& container : containers) {
        for (Program& program : container.programs) {
            by_key.emplace(program.fingerprint ^ (u64{program.offset} * 0x9e3779b97f4a7c15ULL),
                           &program);
        }
    }
    size_t matched = 0;
    for (const ManifestRow& row : rows) {
        bool found = false;
        const auto [first, last] =
            by_key.equal_range(row.fingerprint ^ (u64{row.pool_offset} * 0x9e3779b97f4a7c15ULL));
        for (auto it = first; it != last; ++it) {
            Program& program = *it->second;
            if (program.fingerprint != row.fingerprint || program.offset != row.pool_offset ||
                program.size != row.size || program.stage != row.stage) {
                continue;
            }
            found = true;
            if (row.tier < program.tier) {
                program.tier = row.tier;
            }
            if (!row.bank1_words.empty() && program.manifest_bank1 == ~0U &&
                row.bank1_offset == program.constants_offset) {
                program.manifest_bank1 = static_cast<u32>(manifest_bank1_words.size());
                manifest_bank1_words.push_back(row.bank1_words);
            }
        }
        matched += found ? 1 : 0;
    }
    return matched;
}

ShaderPoolIndex::LoadedManifests ShaderPoolIndex::ReadManifests(std::string path) {
    LoadedManifests loaded;
    const auto begin = std::chrono::steady_clock::now();
    std::vector<std::filesystem::path> files;
    std::error_code error;
    const std::filesystem::path root{path};
    if (std::filesystem::is_directory(root, error)) {
        for (const auto& item : std::filesystem::directory_iterator(root, error)) {
            const std::string name = item.path().filename().string();
            if (item.is_regular_file(error) && name.starts_with("manifest-") &&
                name.ends_with(".json") && !name.ends_with(".summary.json")) {
                files.push_back(item.path());
            }
        }
        std::ranges::sort(files);
    } else {
        files.push_back(root);
    }
    for (const auto& file_path : files) {
        std::ifstream file{file_path, std::ios::binary | std::ios::ate};
        if (!file.is_open()) {
            LOG_WARNING(HW_GPU, "AOT manifest: cannot open {}", file_path.string());
            continue;
        }
        std::string text(static_cast<size_t>(file.tellg()), '\0');
        file.seekg(0);
        file.read(text.data(), static_cast<std::streamsize>(text.size()));
        std::vector<ManifestRow> rows;
        if (!file || !ParseManifest(text, rows)) {
            LOG_WARNING(HW_GPU, "AOT manifest: {} is not a valid manifest", file_path.string());
            continue;
        }
        ++loaded.files;
        loaded.rows.insert(loaded.rows.end(), std::make_move_iterator(rows.begin()),
                           std::make_move_iterator(rows.end()));
    }
    loaded.time = std::chrono::steady_clock::now() - begin;
    return loaded;
}

void ShaderPoolIndex::StartManifestLoad() {
    const char* path = std::getenv("EDEN_NX2_AOT_MANIFEST");
    if (path == nullptr || path[0] == '\0') {
        return;
    }
    // Parsed on its own thread (a directory may hold a manifest per scene); recognition waits
    // for it, so no pool is offered in the wrong order.
    manifest_load = std::async(std::launch::async, &ReadManifests, std::string{path});
    manifest_pending = true;
}

bool ShaderPoolIndex::PollManifest() {
    if (!manifest_pending) {
        return true;
    }
    if (manifest_load.wait_for(std::chrono::seconds{0}) != std::future_status::ready) {
        return false;
    }
    manifest_pending = false;
    const auto apply_begin = std::chrono::steady_clock::now();
    LoadedManifests loaded = manifest_load.get();
    manifest_files = loaded.files;
    manifest_rows = loaded.rows.size();
    manifest_rows_matched = ApplyManifest(loaded.rows);
    for (const auto& words : manifest_bank1_words) {
        manifest_bytes += words.capacity() * sizeof(u32) + sizeof(words);
    }
    manifest_time = std::chrono::steady_clock::now() - apply_begin;
    // With a manifest, the siblings it does not name are not offered (run 20261106-020645:
    // 39 of 1418 such seeds were drawn, against 236 of 792 manifest seeds, for 64 % of the
    // worker time). EDEN_NX2_AOT_OTHERS=1 offers them after the manifest programs.
    const char* others = std::getenv("EDEN_NX2_AOT_OTHERS");
    offer_others = manifest_files == 0 || (others != nullptr && others[0] == '1');
    LOG_INFO(HW_GPU,
             "AOT manifest: {} files, {} rows of tiers 1-3, {} matched indexed programs, {} with "
             "bank-1 words ({} bytes); read in {:.1f} ms on a loader thread, applied in {:.1f} ms; "
             "{} first uses waited for it; other siblings {}",
             manifest_files, manifest_rows, manifest_rows_matched, manifest_bank1_words.size(),
             manifest_bytes, std::chrono::duration<double, std::milli>(loaded.time).count(),
             std::chrono::duration<double, std::milli>(manifest_time).count(),
             deferred_first_uses.size(), offer_others ? "offered after them" : "not offered");
    return true;
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
    auto index = std::unique_ptr<ShaderPoolIndex>(new ShaderPoolIndex(std::move(containers)));
    index->StartManifestLoad();
    return index;
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
             candidates_queued, std::chrono::duration<double, std::milli>(time).count(),
             std::chrono::duration<double, std::milli>(seed_time).count());
    if (manifest_files == 0) {
        return;
    }
    const auto tiers = [](const std::array<u64, PriorityCount>& counts) {
        return fmt::format("{}/{}/{}/{}", counts[0], counts[1], counts[2], counts[3]);
    };
    LOG_INFO(HW_GPU,
             "AOT manifest ({}): by tier 1/2/3/other: candidates offered {}, seeded {}, first "
             "uses of verified programs {}, of them still queued {}; queue peak {}; bank-1 "
             "words agreed with the pool {}, differed {}, not checked {}",
             when, tiers(offered_by_tier), tiers(seeded), tiers(first_uses_by_tier),
             tiers(first_uses_still_queued), candidates_queued_peak, bank1_agreed,
             bank1_disagreed, bank1_unchecked);
}

void ShaderPoolIndex::CheckBank1(const Candidate& candidate, std::span<const u8> constants) {
    if (candidate.manifest_bank1 >= manifest_bank1_words.size()) {
        return;
    }
    const auto& words = manifest_bank1_words[candidate.manifest_bank1];
    if (constants.size() < words.size() * sizeof(u32)) {
        ++bank1_unchecked;
        return;
    }
    if (std::memcmp(constants.data(), words.data(), words.size() * sizeof(u32)) == 0) {
        ++bank1_agreed;
    } else {
        ++bank1_disagreed;
    }
}

std::vector<ShaderPoolIndex::Candidate> ShaderPoolIndex::TakeCandidates(size_t count,
                                                                         bool manifest_only) {
    std::vector<Candidate> result;
    const size_t priorities = manifest_only ? PriorityCount - 1 : PriorityCount;
    for (size_t priority = 0; priority < priorities && result.size() < count; ++priority) {
        auto& queue = candidates_queues[priority];
        while (result.size() < count && !queue.empty()) {
            const Candidate candidate = queue.front();
            queue.pop_front();
            --candidates_queued;
            if (candidate.tier != NoTier) {
                --manifest_queued;
            }
            queued_tier.erase(candidate.address);
            if (!used.contains(candidate.address)) {
                result.push_back(candidate);
            }
        }
    }
    return result;
}

void ShaderPoolIndex::OnFirstUse(Tegra::MemoryManager& gpu_memory, GPUVAddr address) {
    if (manifest_pending) {
        if (!PollManifest()) {
            if (deferred_first_uses.size() < MaxCandidatesQueued) {
                deferred_first_uses.push_back(address);
            }
            return;
        }
        // The first uses seen while the manifest loaded, now in the manifest's order.
        for (const GPUVAddr deferred : std::exchange(deferred_first_uses, {})) {
            OnFirstUse(gpu_memory, deferred);
        }
    }
    const auto begin = std::chrono::steady_clock::now();
    ++first_uses;
    if (const auto it = predicted.find(address); it != predicted.end()) {
        ++first_uses_predicted;
        ++first_uses_by_tier[it->second - 1];
        predicted.erase(it);
        if (const auto queued = queued_tier.find(address); queued != queued_tier.end()) {
            ++first_uses_still_queued[queued->second - 1];
        }
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
            if (manifest_files != 0) {
                std::array<size_t, PriorityCount> tiers{};
                for (const auto& [where, program] : found) {
                    tiers[program->tier - 1] += program->stage == 0 || program->stage == 4 ? 1 : 0;
                }
                // When each pool was recognised, against the run's timeline (seeds have to be
                // ready before the scene's draws).
                LOG_INFO(HW_GPU,
                         "AOT pool recognised: {} of {} programs verified at {:#x}; vertex and "
                         "pixel programs by tier 1/2/3/other {}/{}/{}/{}",
                         verified, total, pool, tiers[0], tiers[1], tiers[2], tiers[3]);
            }
            siblings_verified += verified;
            siblings_mismatched += mismatched;
            siblings_unmapped += unmapped;
            // Manifest programs first, lowest tier first; container order within a tier.
            std::ranges::stable_sort(found, {}, [](const auto& item) { return item.second->tier; });
            for (const auto& [where, program] : found) {
                const u8 stage = program->stage;
                if (used.contains(where)) {
                    continue;
                }
                predicted.emplace(where, program->tier);
                // Vertex and pixel programs only: their container slot maps to one pipeline slot.
                if (stage != 0 && stage != 4) {
                    continue;
                }
                if (queued_tier.contains(where) || (program->tier == NoTier && !offer_others)) {
                    continue;
                }
                // A full queue gives way to a manifest program: the newest other sibling goes.
                auto& others = candidates_queues[NoTier - 1];
                if (candidates_queued >= MaxCandidatesQueued && program->tier != NoTier &&
                    !others.empty()) {
                    queued_tier.erase(others.back().address);
                    others.pop_back();
                    --candidates_queued;
                    ++candidates_dropped;
                }
                if (candidates_queued < MaxCandidatesQueued) {
                    candidates_queues[program->tier - 1].push_back(
                        {where, stage, program->tier, program->constants_offset,
                         program->constants_size, program->manifest_bank1});
                    queued_tier.emplace(where, program->tier);
                    ++candidates_queued;
                    if (program->tier != NoTier) {
                        ++manifest_queued;
                    }
                    candidates_queued_peak = (std::max)(candidates_queued_peak, candidates_queued);
                    ++candidates_offered;
                    ++offered_by_tier[program->tier - 1];
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
