// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <compare>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "common/common_types.h"

namespace Core::OotGameDataTrace { struct Profile; }

namespace Core::OotCoop::World {
enum class OriginKind { None, Chest, Bit, Token };
struct Origin {
    OriginKind kind{OriginKind::None};
    u64 key{}, partner{};
    static Origin Chest(u32 key) { return key ? Origin{OriginKind::Chest, key, 0} : Origin{}; }
    explicit operator bool() const { return kind != OriginKind::None; }
    auto operator<=>(const Origin&) const = default;
};
bool Valid(Origin origin);
void EncodeOrigin(nlohmann::json& packet, Origin origin);
std::optional<Origin> DecodeOrigin(const nlohmann::json& packet);
bool OriginField(std::string_view field);

/// Loaded-collectible cleanup (receiver): a bounded walk over the game's placement records to the
/// live actor whose instance key is K, then the game's own death request for it. Every field
/// comes from the private profile's "cleanup" object; this code only knows the shape.
struct Resolver {
    u64 root_cell{};                                  ///< module offset of the record manager cell
    std::vector<std::pair<u64, u64>> groups;          ///< {u32 count field, block array field}
    u64 block_stride{}, block_capacity{}, block_records{}, block_used{};
    u64 record_stride{}, record_flags{}, record_key{}, record_keyed{}, record_actor{};
    u32 in_use{}, keyed{};                            ///< masks in record_flags / record_keyed
    u64 list_head{}, list_sentinel{}, list_next{}, list_back{}; ///< optional intrusive list
    u64 actor_record{}, actor_lifecycle{}, actor_refs{}, actor_flags{};
    u32 live_lo{}, live_hi{}, deleting{};             ///< lifecycle range a handle is given for
    std::array<u64, 4> link{};                        ///< {record field, id, record's actor, record's id}
    std::array<u64, 2> death_chain{};                 ///< death component = *(*(actor + c0) + c1)
    u64 death_actor{}, death_state{}, death_flags{};
    u32 death_disabled{}, reason{};
    u32 max_blocks{64}, max_records{4096}, max_total{65536}, max_list{2048};
    explicit operator bool() const { return root_cell != 0; }
};
bool DecodeResolver(const nlohmann::json& value, Resolver& resolver);

enum class Family { Rupee, Heart, Piece, Container, TokenH, TokenPartner };
enum class Cleanup { FlagOnly, Death };
enum class Reconcile { Off, TargetCurrent };
struct Config {
    std::map<u64, Family> families;
    std::map<u64, u64> pairs;
    u64 actor_off{}, metadata_off{}, key_off{}, shop_lr{}, pending_off{};
    u32 capacity_hash{}, counter_hash{}, ownership_hash{};
    bool hearts{}, tokens{};
    Cleanup cleanup{Cleanup::FlagOnly};
    Resolver resolver;
    /// Families whose loaded actors the receiver may remove (ordinary currency; hearts if shared).
    bool Cleans(u64 key) const;
    Reconcile reconcile{Reconcile::Off};
    bool AdmitsFlag(u64 key) const;
    std::optional<Origin> Source(u64 key) const;
    bool Admits(Origin origin) const;
};
bool DecodeProfileSection(std::string_view field, const nlohmann::json& value,
                          OotGameDataTrace::Profile& profile);
bool ValidateProfile(const OotGameDataTrace::Profile& profile);
bool DecodeConfig(const nlohmann::json& value, Config& config);
std::optional<u64> Number(const nlohmann::json& value, u64 maximum);
std::vector<u64> Flags(Origin origin);

// Receipts and reservations are distinct from flags. A flag-only baseline never awards an item.
// First benefit reservation wins on this client; concurrent local winners keep their own benefit.
class Claims {
public:
    static constexpr size_t Limit = 4096;
    bool Reserve(Origin origin, std::string sender);
    bool Complete(Origin origin, std::string_view sender);
    void Cancel(Origin origin, std::string_view sender);
    bool Applied(Origin origin) const { return applied.contains(origin); }
    bool Busy(Origin origin) const { return pending.contains(origin) || Applied(origin); }
    std::optional<Origin> GaveFlag(u64 key) const;
    void Clear() { pending.clear(); applied.clear(); }
private:
    std::map<Origin, std::string> pending;
    std::set<Origin> applied;
};

struct Capture {
    Origin origin;
    u64 thread{}, epoch{}, tick{}, sp{}; ///< sp: the stack pointer at the component entry
};
// Store identities, never guest actor pointers. One component invocation per thread; bounded age.
// A give consumes the capture only when it runs nested in that invocation (a lower stack).
class Pickups {
public:
    static constexpr u64 StackSpan = 0x10000;
    void Put(Capture capture);
    std::optional<Origin> Take(u64 thread, u64 epoch, u64 tick, u64 sp);
    void Clear() { captures.clear(); }
private:
    std::map<u64, Capture> captures;
};

template <typename Memory>
std::optional<u64> PickupKey(Memory& memory, u64 component, const Config& config) {
    const auto read = [&](u64 ptr, u64 offset) -> std::optional<u64> {
        if (!ptr || ptr > std::numeric_limits<u64>::max() - offset - sizeof(u64) ||
            !memory.IsValidVirtualAddressRange(ptr + offset, sizeof(u64))) return {};
        return memory.Read64(ptr + offset);
    };
    if (!config.actor_off || !config.metadata_off || !config.key_off) return {};
    const auto actor = read(component, config.actor_off);
    const auto metadata = actor ? read(*actor, config.metadata_off) : std::nullopt;
    const auto key = metadata ? read(*metadata, config.key_off) : std::nullopt;
    return key && config.families.contains(*key) ? key : std::nullopt;
}


/// One placement record holding K and what its attached actor allows.
struct Target {
    u64 record{}, actor{};
    std::string_view state; ///< "callable", "unloaded", "detached", or the first failed check
    bool Callable() const { return state == "callable"; }
};
struct Walk {
    std::vector<Target> targets; ///< up to 4 (child/adult placements can share a key)
    u64 blocks{}, records{}, list{};
    bool bounded{}; ///< a cap was hit (or a structure looked corrupt)
};

namespace Detail {
template <typename Memory>
std::optional<u64> Read(Memory& memory, u64 address, size_t size) {
    if (!address || address > std::numeric_limits<u64>::max() - size ||
        !memory.IsValidVirtualAddressRange(address, size)) return {};
    return size == 8 ? memory.Read64(address) : u64{memory.Read32(address)};
}
} // namespace Detail

/// Checks a matched record's actor with pure reads (re-run just before any call).
template <typename Memory>
Target Check(Memory& memory, const Resolver& r, u64 record) {
    const auto r64 = [&](u64 a) { return Detail::Read(memory, a, 8); };
    const auto r32 = [&](u64 a) { return Detail::Read(memory, a, 4); };
    Target t{record, 0, "unreadable"};
    const auto keyed = r32(record + r.record_keyed);
    if (!keyed) return t;
    if (!(*keyed & r.keyed)) return t.state = "no key flag", t;
    const auto tagged = r64(record + r.record_actor);
    if (!tagged) return t;
    if (*tagged == 0) return t.state = "unloaded", t;
    if (*tagged & 1) return t.state = "detached", t;
    t.actor = *tagged & ~u64{3};
    if (!t.actor || (t.actor & 7)) return t.state = "bad actor", t;
    if (r64(t.actor + r.actor_record) != record) return t.state = "back link", t;
    const auto life = r32(t.actor + r.actor_lifecycle);
    if (!life || *life < r.live_lo || *life > r.live_hi) return t.state = "lifecycle", t;
    const auto refs = r32(t.actor + r.actor_refs);
    if (!refs || *refs == 0xFFFFFFFF) return t.state = "destroying", t;
    const auto lr = r64(t.actor + r.link[0]);
    const auto id = r32(t.actor + r.link[1]);
    if (!lr || !*lr || !id || r64(*lr + r.link[2]) != t.actor || r32(*lr + r.link[3]) != id)
        return t.state = "link", t;
    const auto flags = r32(t.actor + r.actor_flags);
    if (!flags || (*flags & r.deleting)) return t.state = "delete requested", t;
    const auto table = r64(t.actor + r.death_chain[0]);
    const auto death = table && *table ? r64(*table + r.death_chain[1]) : std::nullopt;
    if (!death || !*death || r64(*death + r.death_actor) != t.actor) return t.state = "death component", t;
    const auto state = r32(*death + r.death_state), dflags = r32(*death + r.death_flags);
    if (!state || !dflags) return t.state = "death component", t;
    if ((*state & 1) && ((*state >> 4) & 0xF) == r.reason) return t.state = "death pending", t;
    if (*dflags & r.death_disabled) return t.state = "death disabled", t;
    t.state = "callable";
    return t;
}

/// Finds the records holding `key`: the block arrays first, then the list (both bounded).
template <typename Memory>
Walk Find(Memory& memory, u64 base, const Resolver& r, u64 key) {
    const auto r64 = [&](u64 a) { return Detail::Read(memory, a, 8); };
    const auto r32 = [&](u64 a) { return Detail::Read(memory, a, 4); };
    Walk walk;
    const auto match = [&](u64 record) {
        const auto flags = r32(record + r.record_flags);
        if (!flags || !(*flags & r.in_use) || r64(record + r.record_key) != key) return;
        if (walk.targets.size() < 4) walk.targets.push_back(Check(memory, r, record));
    };
    const auto manager = r64(base + r.root_cell);
    if (!manager || !*manager || (*manager & 7)) return walk.bounded = true, walk;
    for (const auto& [count_field, array_field] : r.groups) {
        const auto count = r32(*manager + count_field);
        const auto array = r64(*manager + array_field);
        if (!count || !array || *count > r.max_blocks) { walk.bounded = true; continue; }
        for (u64 b = 0; b < *count && *array; ++b) {
            const u64 block = *array + b * r.block_stride;
            const auto capacity = r32(block + r.block_capacity), used = r32(block + r.block_used);
            const auto records = r64(block + r.block_records);
            ++walk.blocks;
            if (!capacity || !used || !records || !*records || *used > *capacity ||
                *capacity > r.max_records) { walk.bounded |= !used || !capacity || *used > *capacity ||
                                              *capacity > r.max_records; continue; }
            for (u64 i = 0; i < *used; ++i) {
                if (++walk.records > r.max_total) return walk.bounded = true, walk;
                match(*records + i * r.record_stride);
            }
        }
    }
    if (!walk.targets.empty() || !r.list_head) return walk;
    auto node = r64(*manager + r.list_head);
    while (node && *node && *node != *manager + r.list_sentinel) {
        if (++walk.list > r.max_list || *node < r.list_back) return walk.bounded = true, walk;
        match(*node - r.list_back);
        node = r64(*node + r.list_next);
    }
    if (!node) walk.bounded = true;
    return walk;
}

struct Wallet {
    s32 balance{}, capacity{};
    bool operator==(const Wallet&) const = default;
};
std::optional<Wallet> DecodeWallet(const nlohmann::json& value);
nlohmann::json EncodeWallet(Wallet wallet);
std::optional<s64> ReconcileDelta(Wallet ours, Wallet peer, Reconcile mode);
} // namespace Core::OotCoop::World
