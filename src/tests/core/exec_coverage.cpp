// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "core/arm/exec_coverage.h"

namespace {

using Core::ExecCoverage::Map;
using Core::ExecCoverage::Range;

constexpr u64 Base = 0x80004000;

/// What the JIT's block prologue does on entry.
void Enter(u8* byte, u8 mask) {
    REQUIRE(byte != nullptr);
    *byte |= mask;
}

} // Anonymous namespace

TEST_CASE("Coverage only instruments blocks that start in the module", "[core][exec_coverage]") {
    Map map{Base, 0x1000};
    u8 mask = 0;
    REQUIRE(map.OnBlockCompiled(Base - 4, Base + 8, mask) == nullptr);
    REQUIRE(map.OnBlockCompiled(Base + 0x1000, Base + 0x1010, mask) == nullptr);
    u8* const first = map.OnBlockCompiled(Base, Base + 8, mask);
    REQUIRE(first != nullptr);
    REQUIRE(mask == 0x01);
    // Slot 9 (offset 0x24) is bit 1 of byte 1.
    u8* const ninth = map.OnBlockCompiled(Base + 0x24, Base + 0x28, mask);
    REQUIRE(ninth == first + 1);
    REQUIRE(mask == 0x02);
    REQUIRE(map.CompiledBlocks() == 2);
}

TEST_CASE("Coverage ranges merge hit blocks and widen them to their extents", "[core][exec_coverage]") {
    Map map{Base, 0x1000};
    u8 mask = 0;
    u8* a = map.OnBlockCompiled(Base + 0x100, Base + 0x110, mask); // 4 instructions
    const u8 mask_a = mask;
    u8* b = map.OnBlockCompiled(Base + 0x110, Base + 0x118, mask); // follows a
    const u8 mask_b = mask;
    u8* c = map.OnBlockCompiled(Base + 0x200, Base + 0x204, mask);
    const u8 mask_c = mask;
    map.OnBlockCompiled(Base + 0x300, Base + 0x340, mask); // compiled, never entered

    REQUIRE(map.Ranges(true).empty());
    Enter(a, mask_a);
    Enter(b, mask_b);
    Enter(c, mask_c);

    REQUIRE(map.Ranges(true) == std::vector<Range>{{0x100, 0x18}, {0x200, 4}});
    REQUIRE(map.Ranges(false) == std::vector<Range>{{0x100, 4}, {0x110, 4}, {0x200, 4}});

    // A recompile with a longer extent widens the block; a shorter one does not shrink it.
    map.OnBlockCompiled(Base + 0x200, Base + 0x210, mask);
    map.OnBlockCompiled(Base + 0x200, Base + 0x208, mask);
    REQUIRE(map.Ranges(true) == std::vector<Range>{{0x100, 0x18}, {0x200, 0x10}});
}

TEST_CASE("Coverage extents stop at the module end", "[core][exec_coverage]") {
    Map map{Base, 0x100};
    u8 mask = 0;
    u8* const byte = map.OnBlockCompiled(Base + 0xF8, Base + 0x120, mask);
    Enter(byte, mask);
    REQUIRE(map.Ranges(true) == std::vector<Range>{{0xF8, 8}});
}

TEST_CASE("Coverage stop freezes snapshots and start clears them", "[core][exec_coverage]") {
    Map map{Base, 0x1000};
    u8 mask = 0;
    u8* const byte = map.OnBlockCompiled(Base + 0x40, Base + 0x48, mask);
    const u8 entered = mask;
    Enter(byte, entered);

    map.Freeze();
    REQUIRE(map.Frozen());
    // The live bits are all set so the prologues stop writing...
    REQUIRE(*byte == 0xFF);
    // ...but snapshots return what had been hit.
    REQUIRE(map.Ranges(true) == std::vector<Range>{{0x40, 8}});
    REQUIRE(map.Bits()[2] == entered);

    map.Clear();
    REQUIRE_FALSE(map.Frozen());
    REQUIRE(map.Ranges(true).empty());
    Enter(byte, entered);
    REQUIRE(map.Ranges(true) == std::vector<Range>{{0x40, 8}});
}

TEST_CASE("Coverage merge works on raw bits", "[core][exec_coverage]") {
    // Bits 0, 1 and 15: slots 0, 1 and 15; slot 0's block is 3 slots long and swallows slot 1.
    const std::vector<u8> bits{0x03, 0x80};
    const std::unordered_map<u32, u32> units{{0, 3}, {15, 1}};
    REQUIRE(Map::RangesFromBits(bits, units, true) == std::vector<Range>{{0, 12}, {60, 4}});
    REQUIRE(Map::RangesFromBits(bits, units, false) == std::vector<Range>{{0, 8}, {60, 4}});
    REQUIRE(Map::RangesFromBits(std::vector<u8>(4), units, true).empty());
}
