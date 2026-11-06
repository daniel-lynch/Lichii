// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdlib>
#include <cstring>
#include <string>

#include "common/logging.h"
#include "core/arm/exec_coverage.h"
#include "core/hle/kernel/k_process.h"

namespace Core::ExecCoverage {

namespace {

u8 LoadByte(u8& byte) {
    return std::atomic_ref<u8>{byte}.load(std::memory_order_relaxed);
}

void StoreByte(u8& byte, u8 value) {
    std::atomic_ref<u8>{byte}.store(value, std::memory_order_relaxed);
}

struct Registry {
    std::mutex mutex;
    std::shared_ptr<Map> current;
    std::vector<std::shared_ptr<Map>> retired;
};

Registry& GetRegistry() {
    static Registry registry;
    return registry;
}

} // Anonymous namespace

Map::Map(u64 base_, u64 size_)
    : base{base_}, size{size_}, bit_bytes{static_cast<size_t>((size_ / Granule + 7) / 8)} {
    bits = std::make_unique<u8[]>(bit_bytes);
    std::memset(bits.get(), 0, bit_bytes);
}

u8* Map::OnBlockCompiled(u64 start_pc, u64 end_pc, u8& mask) {
    if (start_pc < base || start_pc - base >= size) {
        return nullptr;
    }
    const u64 unit = (start_pc - base) / Granule;
    const u64 end = std::clamp<u64>(end_pc, start_pc + Granule, base + size);
    const u32 units = static_cast<u32>((end - start_pc + Granule - 1) / Granule);
    {
        std::scoped_lock lock{mutex};
        u32& known = block_units[static_cast<u32>(unit)];
        known = std::max(known, units);
    }
    mask = static_cast<u8>(1u << (unit % 8));
    return bits.get() + unit / 8;
}

void Map::Clear() {
    std::scoped_lock lock{mutex};
    for (size_t index = 0; index < bit_bytes; ++index) {
        StoreByte(bits[index], 0);
    }
    frozen.clear();
    frozen.shrink_to_fit();
    is_frozen = false;
}

void Map::Freeze() {
    std::scoped_lock lock{mutex};
    if (is_frozen) {
        return;
    }
    frozen.resize(bit_bytes);
    for (size_t index = 0; index < bit_bytes; ++index) {
        frozen[index] = LoadByte(bits[index]);
        StoreByte(bits[index], 0xFF);
    }
    is_frozen = true;
}

bool Map::Frozen() const {
    std::scoped_lock lock{mutex};
    return is_frozen;
}

std::vector<u8> Map::Bits() const {
    std::scoped_lock lock{mutex};
    if (is_frozen) {
        return frozen;
    }
    std::vector<u8> copy(bit_bytes);
    for (size_t index = 0; index < bit_bytes; ++index) {
        copy[index] = LoadByte(bits[index]);
    }
    return copy;
}

std::vector<Range> Map::Ranges(bool extents) const {
    const std::vector<u8> copy = Bits();
    std::scoped_lock lock{mutex};
    return RangesFromBits(copy, block_units, extents);
}

size_t Map::CompiledBlocks() const {
    std::scoped_lock lock{mutex};
    return block_units.size();
}

std::vector<Range> Map::RangesFromBits(std::span<const u8> bits,
                                       const std::unordered_map<u32, u32>& block_units,
                                       bool extents) {
    // Collect [start, end) slot intervals in start order, then merge touching ones.
    std::vector<std::pair<u64, u64>> spans;
    for (size_t byte = 0; byte < bits.size(); ++byte) {
        u8 value = bits[byte];
        while (value != 0) {
            const unsigned bit = static_cast<unsigned>(std::countr_zero(value));
            value = static_cast<u8>(value & (value - 1));
            const u64 unit = byte * 8 + bit;
            u64 units = 1;
            if (extents) {
                if (const auto found = block_units.find(static_cast<u32>(unit));
                    found != block_units.end()) {
                    units = std::max<u64>(found->second, 1);
                }
            }
            spans.emplace_back(unit, unit + units);
        }
    }
    std::vector<Range> ranges;
    u64 run_start = 0;
    u64 run_end = 0;
    bool open = false;
    for (const auto& [start, end] : spans) {
        if (open && start <= run_end) {
            run_end = std::max(run_end, end);
            continue;
        }
        if (open) {
            ranges.push_back({run_start * Granule, (run_end - run_start) * Granule});
        }
        run_start = start;
        run_end = end;
        open = true;
    }
    if (open) {
        ranges.push_back({run_start * Granule, (run_end - run_start) * Granule});
    }
    return ranges;
}

bool Enabled() {
    static const bool enabled = [] {
        const char* const value = std::getenv("EDEN_NX2_COVERAGE");
        return value != nullptr && value[0] != '\0' && std::string_view{value} != "0";
    }();
    return enabled;
}

void OnModuleLoaded(const Kernel::KProcess& process, std::string_view name, u64 base, u64 size) {
    if (!Enabled() || name != "main" || !process.IsApplication()) {
        return;
    }
    auto map = std::make_shared<Map>(base, size);
    auto& registry = GetRegistry();
    std::scoped_lock lock{registry.mutex};
    if (registry.current) {
        registry.retired.push_back(std::move(registry.current));
    }
    registry.current = std::move(map);
    LOG_INFO(Loader, "Execution coverage on for main at {:#x} (size {:#x}, {} KiB bitmap)", base,
             size, (size / Map::Granule / 8 + 1023) / 1024);
}

std::shared_ptr<Map> Current() {
    if (!Enabled()) {
        return nullptr;
    }
    auto& registry = GetRegistry();
    std::scoped_lock lock{registry.mutex};
    return registry.current;
}

} // namespace Core::ExecCoverage
