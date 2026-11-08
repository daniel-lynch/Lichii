// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>

#include "core/arm/dynarmic/oot_gamedata_trace.h"

/// A synthetic private profile for the tests: made-up module offsets, hashes and entry words.
/// None of these is a value from the game (real ones live only in the user-supplied file).
namespace OotTestProfile {

namespace Trace = Core::OotGameDataTrace;

inline constexpr char BuildId[] = "0123456789ABCDEF0123456789ABCDEF01234567";
inline constexpr u64 ManagerCell = 0x200000;
inline constexpr u64 PouchCell = 0x200100;
inline constexpr u32 Wallet = 0x0BADCAFE;
inline constexpr u64 BoolSetter = 0x110000;
inline constexpr u64 BitSetter = 0x111000;
inline constexpr u64 RupeeEntry = 0x112000;
inline constexpr u32 Nop = 0xD503201F;
/// Two arbitrary code words each entry must start with (any instructions the tests can run).
inline constexpr std::array<u32, 2> BoolWords{Trace::Pacibsp, Nop};
inline constexpr std::array<u32, 2> BitWords{Trace::MovX8X0, Nop};
inline constexpr std::array<u32, 2> RupeeWords{Nop, Trace::MovX8X0};

inline Trace::Profile Make() {
    using Trace::Point;
    Trace::Profile profile;
    profile.build_id = BuildId;
    const std::array<std::pair<Point, u64>, 10> sites{{
        {Point::StepSet, 0x100004}, {Point::StepApply, 0x101000},
        {Point::PlayedEvent, 0x102000}, {Point::Frame, 0x103000},
        {Point::BitRead, 0x104004}, {Point::BitSet, 0x105004},
        {Point::BitRead, 0x106004}, {Point::BitSet, 0x107004},
        {Point::StructBoolSet, 0x108000}, {Point::ActorFlagSet, 0x109008}}};
    for (const auto& [point, offset] : sites)
        profile.sites.push_back({point, offset, Trace::ReplacedInstruction(point)});
    profile.manager_cell = ManagerCell;
    profile.pouch_cell = PouchCell;
    profile.wallet_hash = Wallet;
    profile.bool_setter = {BoolSetter, {BoolWords.begin(), BoolWords.end()}};
    profile.bit_setter = {BitSetter, {BitWords.begin(), BitWords.end()}};
    profile.rupee_entry = {RupeeEntry, {RupeeWords.begin(), RupeeWords.end()}};
    return profile;
}

inline const std::shared_ptr<const Trace::Profile>& Shared() {
    static const auto profile = std::make_shared<const Trace::Profile>(Make());
    return profile;
}

inline const Trace::Profile& Get() {
    return *Shared();
}

} // namespace OotTestProfile
