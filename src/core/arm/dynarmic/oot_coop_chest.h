// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include "common/common_types.h"
#include "common/nx2_event_trace.h"
#include "core/arm/dynarmic/oot_coop_world.h"

namespace Core::OotCoop {
struct Change;
namespace Chest {
// Private profile data only: the public implementation contains no game layout constants.
struct Config {
    u64 interaction_pc{}, confirm_pc{}, return_pc{};
    u32 interaction_word{}, confirm_word{}, return_word{}, struct_hash{};
    u64 selector_vtable{}, input_offset{};
    u32 input_value{}, input_pointer_mask{};
    u64 reserved_offset{}, current_offset{}, flags_offset{};
    u8 invalid_child{}, transition_mask{};
    std::array<u64, 3> owner_chain{};
    u64 metadata_offset{}, metadata_key_offset{};
    std::array<u64, 4> link{}; ///< actor record, actor generation, record actor, record generation
    bool enforce{}; ///< Enable suppression only after the hook identity/order probe passes.
};
bool DecodeConfig(const nlohmann::json& value, Config& config);

struct ActorToken {
    u64 actor{}, record{}, lifetime{}, epoch{};
    auto operator<=>(const ActorToken&) const = default;
};
struct EventToken {
    u64 pointer{}, generation{};
    explicit operator bool() const { return pointer && generation; }
    auto operator<=>(const EventToken&) const = default;
};
// Nonzero context changes may be nested events. Only an observed idle manager closes
// the activity interval; an unreadable manager never ends or reuses a binding.
class Events {
public:
    static constexpr size_t Limit = 64;
    EventToken Sample(std::optional<u64> pointer);
    std::vector<EventToken> TakeEnded();
    bool Overflowed() const { return overflowed; }
private:
    std::map<u64, EventToken> active;
    std::vector<EventToken> ended;
    u64 generation{};
    bool overflowed{};
};
struct Identity {
    ActorToken token;
    u64 instance{};
    u32 member{}, key{};
};

namespace Detail {
template <typename Memory>
std::optional<u64> Read(Memory& memory, u64 pointer, u64 offset, size_t bytes) {
    constexpr u64 Max = std::numeric_limits<u64>::max();
    if (!pointer || offset > Max - pointer || bytes > Max - (pointer + offset) ||
        !memory.IsValidVirtualAddressRange(pointer + offset, bytes)) return {};
    if (bytes == 1) return memory.Read8(pointer + offset);
    if (bytes == 4) return memory.Read32(pointer + offset);
    return memory.Read64(pointer + offset);
}
} // namespace Detail

template <typename Memory>
std::optional<u64> ReadNodeActor(Memory& memory, u64 node, const Config& config) {
    for (const auto offset : config.owner_chain) {
        const auto next = Detail::Read(memory, node, offset, sizeof(u64));
        if (!next || !*next) return {};
        node = *next;
    }
    return node;
}

template <typename Memory>
std::optional<Identity> ReadActor(Memory& memory, u64 actor, u64 epoch, const Config& config,
                                const std::map<u32, u32>& members) {
    const auto metadata = Detail::Read(memory, actor, config.metadata_offset, sizeof(u64));
    const auto record = Detail::Read(memory, actor, config.link[0], sizeof(u64));
    const auto lifetime = Detail::Read(memory, actor, config.link[1], sizeof(u32));
    if (!metadata || !*metadata || !record || !*record || !lifetime ||
        Detail::Read(memory, *record, config.link[2], sizeof(u64)) != actor ||
        Detail::Read(memory, *record, config.link[3], sizeof(u32)) != lifetime) return {};
    const auto instance = Detail::Read(memory, *metadata, config.metadata_key_offset, sizeof(u64));
    if (!instance) return {};
    const u32 member = Common::Nx2EventTrace::Murmur3(std::to_string(*instance));
    const auto found = members.find(member);
    if (found == members.end() || !found->second) return {};
    return Identity{{actor, *record, *lifetime, epoch}, *instance, member, found->second};
}

template <typename Memory>
bool SelectedInteraction(Memory& memory, u64 node, u64 base, const Config& config) {
    if (!config.selector_vtable || base > std::numeric_limits<u64>::max() - config.selector_vtable ||
        Detail::Read(memory, node, 0, sizeof(u64)) != base + config.selector_vtable) return false;
    const auto input = Detail::Read(memory, node, config.input_offset, sizeof(u64));
    if (!input || Detail::Read(memory, *input & ~u64{config.input_pointer_mask}, 0, sizeof(u32)) !=
                      config.input_value) return false;
    const auto reserved = Detail::Read(memory, node, config.reserved_offset, 1);
    const auto current = Detail::Read(memory, node, config.current_offset, 1);
    const auto flags = Detail::Read(memory, node, config.flags_offset, 1);
    return reserved && current && flags && *reserved != config.invalid_child &&
           *reserved != *current && !(*flags & config.transition_mask);
}

template <typename Memory>
std::optional<Identity> ReadInteraction(Memory& memory, u64 node, u64 base, u64 epoch,
                                      const Config& config, const std::map<u32, u32>& members) {
    if (!SelectedInteraction(memory, node, base, config)) return {};
    const auto actor = ReadNodeActor(memory, node, config);
    return actor ? ReadActor(memory, *actor, epoch, config, members) : std::nullopt;
}

enum class Outcome { Pending, LocalApplied, RemoteApplying, RemoteApplied, Unknown, Historical };
enum class Decision { Allow, Skip, Unknown };
struct GateResult {
    Decision decision{Decision::Unknown};
    u64 transaction{};
    u32 part{};
};
struct ChestRecord {
    std::string source; ///< Empty means local; selected distinguishes an unclaimed record.
    bool selected{}, closed{}, unresolved{}, transaction_selected{};
    u64 attempt{}, claim_order{}, transaction{};
    Outcome outcome{Outcome::Pending};
    std::set<u32> applied, applying;
    std::optional<u32> final_part;
    std::optional<u64> pending_since;
    bool missing_logged{};
};
struct Attempt {
    u64 id{}, tick{};
    Identity identity;
    std::optional<EventToken> event;
    u32 next_part{};
    bool ended{}, unresolved{};
    std::set<u32> applying;
};

// Serialized by Session's existing mutex. Receipts survive event end and actor replacement.
class Claims {
public:
    static constexpr size_t Limit = 4096;
    static constexpr u32 PartLimit = 64;
    u64 LocalBegin(const Identity& identity, u64 epoch, u64 tick);
    bool BindEvent(u64 attempt, EventToken event);
    bool LocalActive(u32 key) const;
    std::optional<u64> FindActor(ActorToken actor) const;
    std::optional<u64> FindEvent(EventToken event) const;
    std::optional<u64> Attribute(std::optional<ActorToken> actor, EventToken event) const;
    GateResult Gate(u64 attempt);
    bool Returned(u64 attempt, u32 part, bool success);
    void Complete(u32 key);
    void EndEvent(EventToken event);
    /// Ends unconfirmed attempts older than `age` (released when unpaid); returns their ids.
    std::vector<u64> Expire(u64 tick, u64 age = 1800);
    bool Active() const; ///< Any attempt not yet ended.
    std::vector<u32> MissingBenefits(u64 tick, u64 age = 1800);
    void RemoteClaim(World::Origin key, const std::string& origin, bool baseline = false);
    bool AcceptRemote(const Change& change);
    bool CanApply(const Change& change) const;
    bool StartRemote(const Change& change);
    void RemoteReturned(const Change& change, bool success);
    std::string_view Owner(World::Origin key) const;
    const ChestRecord* Find(u32 key) const;
    const Attempt* FindAttempt(u64 id) const;
    size_t Size() const { return records.size(); }
    void Reset();
private:
    ChestRecord* Get(u32 key);
    void SelectLocal(ChestRecord& record, u64 id);
    void End(u64 id);
    std::map<u32, ChestRecord> records;
    std::map<u64, Attempt> attempts;
    std::map<ActorToken, u64> actors;
    std::map<EventToken, u64> events;
    std::set<EventToken> ambiguous;
    u64 next_attempt{}, next_order{};
};
} // namespace Chest
} // namespace Core::OotCoop
