// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

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
    u64 thread{}, epoch{}, tick{}, event{};
};
// Store identities, never guest actor pointers. One component invocation per thread; bounded age.
class Pickups {
public:
    void Put(Capture capture);
    std::optional<Origin> Take(u64 thread, u64 epoch, u64 tick);
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

struct Wallet {
    s32 balance{}, capacity{};
    bool operator==(const Wallet&) const = default;
};
std::optional<Wallet> DecodeWallet(const nlohmann::json& value);
nlohmann::json EncodeWallet(Wallet wallet);
std::optional<s64> ReconcileDelta(Wallet ours, Wallet peer, Reconcile mode);
} // namespace Core::OotCoop::World
