// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"

namespace Kernel {
class KProcess;
}

// Execution coverage of the application's `main` module (docs/nx2/exec-coverage.md).
//
// EDEN_NX2_COVERAGE=1 makes the JIT compile every guest block that starts inside `main` with a
// prologue that sets one bit per 4-byte instruction slot the first time the block is entered.
// The debug server's coverage_* tools read, clear and freeze the map. Off (the default), nothing
// is allocated and no code is emitted.
namespace Core::ExecCoverage {

/// A run of hit code, as an offset from the module base and a length, both in bytes.
struct Range {
    u64 offset{};
    u64 length{};
    bool operator==(const Range&) const = default;
};

/// The hit bitmap of one module and the extent of every block compiled in it.
class Map {
public:
    /// Bytes of guest code per bit: one AArch64 instruction.
    static constexpr u64 Granule = 4;

    Map(u64 base, u64 size);

    Map(const Map&) = delete;
    Map& operator=(const Map&) = delete;

    [[nodiscard]] u64 Base() const {
        return base;
    }
    [[nodiscard]] u64 Size() const {
        return size;
    }

    /// The JIT compiled a block [start_pc, end_pc). Returns the byte the block's prologue marks
    /// and sets mask, or nullptr when the block does not start inside the module.
    u8* OnBlockCompiled(u64 start_pc, u64 end_pc, u8& mask);

    /// Clears every bit and resumes recording (coverage_start, coverage_reset).
    void Clear();
    /// Keeps a copy of the bits for snapshots, then sets every live bit so the compiled
    /// prologues stop writing (coverage_stop). Clear() resumes.
    void Freeze();
    [[nodiscard]] bool Frozen() const;

    /// The bits now (or as frozen): bit i of byte b is the slot at offset (b * 8 + i) * Granule.
    [[nodiscard]] std::vector<u8> Bits() const;

    /// Hit code as merged ranges. With extents, each hit block start covers the whole compiled
    /// block; without, only the 4-byte start slot of each hit block.
    [[nodiscard]] std::vector<Range> Ranges(bool extents) const;

    [[nodiscard]] size_t CompiledBlocks() const;

    /// The merge behind Ranges(), separate so it can be tested without a JIT. block_units maps
    /// a block's start slot to its length in slots.
    [[nodiscard]] static std::vector<Range> RangesFromBits(
        std::span<const u8> bits, const std::unordered_map<u32, u32>& block_units, bool extents);

private:
    u64 base;
    u64 size;
    std::unique_ptr<u8[]> bits;
    size_t bit_bytes;
    mutable std::mutex mutex;
    std::unordered_map<u32, u32> block_units;
    std::vector<u8> frozen;
    bool is_frozen{};
};

/// EDEN_NX2_COVERAGE is set (read once).
[[nodiscard]] bool Enabled();

/// The loader placed a module; only the application's `main` gets a map, replacing any earlier
/// one (whose memory is kept, as code compiled against it may still exist).
void OnModuleLoaded(const Kernel::KProcess& process, std::string_view name, u64 base, u64 size);

/// The map of the current application's `main`, or null.
[[nodiscard]] std::shared_ptr<Map> Current();

} // namespace Core::ExecCoverage
