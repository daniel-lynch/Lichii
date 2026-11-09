// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The local (resident) player actor, found by pure reads: a manager cell in the main module
// holds the registered equipment user, whose owner backlink is the player actor. The actor is
// validated like any actor (record backlink, generation, lifecycle, holder -> the same user,
// name). Every offset and name comes from the private key file; nothing here is game data.

#include <limits>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include "common/common_types.h"

namespace Core::OotCoop::Player {

struct Keys {
    u64 user_cell{};      ///< main-relative cell: pointer to the manager
    u64 user_equipment{}; ///< manager -> registered equipment user
    u64 owner{};          ///< equipment user -> owning actor
    u64 holder{};         ///< actor -> component holder
    u64 equipment{};      ///< holder -> the actor's equipment user (must equal the registered one)
    u64 actor_record{}, actor_id{}, record_actor{}, record_id{}, actor_name{}, lifecycle{};
    u32 lifecycle_lo{}, lifecycle_hi{}; ///< inclusive lifecycle range that means teardown
    std::vector<std::string> names;    ///< accepted resident player actor names
};
/// The private key-file object; unknown or missing fields reject it.
[[nodiscard]] std::optional<Keys> DecodeKeys(const nlohmann::json& value);

struct Resident {
    u64 actor{}, record{}, equipment{};
    u32 generation{};
    bool operator==(const Resident&) const = default;
};

namespace Detail {
template <typename Memory>
std::optional<u64> Read(Memory& memory, u64 base, u64 offset, size_t size) {
    if (!base || offset > std::numeric_limits<u64>::max() - base) return std::nullopt;
    const u64 address = base + offset;
    if (address > std::numeric_limits<u64>::max() - size ||
        !memory.IsValidVirtualAddressRange(address, size)) return std::nullopt;
    return size == 8 ? u64{memory.Read64(address)} : u64{memory.Read32(address)};
}
template <typename Memory>
std::optional<std::string> Name(Memory& memory, u64 address) {
    std::string out;
    for (u64 i = 0; i < 64 && address && address <= std::numeric_limits<u64>::max() - i; ++i) {
        if (!memory.IsValidVirtualAddressRange(address + i, 1)) return std::nullopt;
        const u8 c = memory.Read8(address + i);
        if (!c) return out.empty() ? std::nullopt : std::optional<std::string>{out};
        if (c < 32 || c > 126) return std::nullopt;
        out.push_back(static_cast<char>(c));
    }
    return std::nullopt;
}
} // namespace Detail

/// Rechecks a resident found earlier: same registered user, same owner, record backlink,
/// generation, live lifecycle, holder -> user, accepted name. Call before and after every use.
template <typename Memory>
bool Validate(Memory& memory, u64 base, const Resident& r, const Keys& k) {
    using Detail::Read;
    const auto manager = Read(memory, base, k.user_cell, 8);
    if (!manager || !r.actor || !r.record || !r.equipment ||
        Read(memory, *manager, k.user_equipment, 8) != r.equipment ||
        Read(memory, r.equipment, k.owner, 8) != r.actor) return false;
    const auto holder = Read(memory, r.actor, k.holder, 8);
    const auto life = Read(memory, r.actor, k.lifecycle, 4);
    if (!holder || Read(memory, *holder, k.equipment, 8) != r.equipment ||
        Read(memory, r.actor, k.actor_record, 8) != r.record ||
        Read(memory, r.actor, k.actor_id, 4) != u64{r.generation} || r.generation == 0xFFFFFFFF ||
        Read(memory, r.record, k.record_actor, 8) != r.actor ||
        Read(memory, r.record, k.record_id, 4) != u64{r.generation} || !life ||
        (*life >= k.lifecycle_lo && *life <= k.lifecycle_hi)) return false;
    const auto name_pointer = Read(memory, r.actor, k.actor_name, 8);
    const auto name = name_pointer ? Detail::Name(memory, *name_pointer) : std::nullopt;
    if (!name) return false;
    for (const auto& accepted : k.names)
        if (*name == accepted) return true;
    return false;
}

template <typename Memory>
std::optional<Resident> Find(Memory& memory, u64 base, const Keys& k) {
    using Detail::Read;
    const auto manager = Read(memory, base, k.user_cell, 8);
    const auto equipment = manager ? Read(memory, *manager, k.user_equipment, 8) : std::nullopt;
    const auto actor = equipment ? Read(memory, *equipment, k.owner, 8) : std::nullopt;
    if (!actor) return std::nullopt;
    const auto record = Read(memory, *actor, k.actor_record, 8);
    const auto generation = Read(memory, *actor, k.actor_id, 4);
    if (!record || !generation) return std::nullopt;
    const Resident resident{*actor, *record, *equipment, static_cast<u32>(*generation)};
    return Validate(memory, base, resident, k) ? std::optional<Resident>{resident} : std::nullopt;
}

} // namespace Core::OotCoop::Player
