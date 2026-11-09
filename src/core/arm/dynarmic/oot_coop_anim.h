// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <bit>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>
#include "common/common_types.h"
#include "core/arm/dynarmic/oot_gamedata_trace.h"

namespace Core::OotCoop::Anim {

inline constexpr u32 Revision = 1;
inline constexpr u32 CommandLimit = 471;
inline constexpr u32 IdLimit = std::numeric_limits<u32>::max();
inline constexpr u32 UnknownCommand = 0xFFFF; ///< Snapshot slot without a normal command
inline constexpr size_t TableEntryLimit = 16384;
inline constexpr size_t TableByteLimit = 8U << 20;

struct State {
    u32 aid{}, aseq{};
    std::optional<float> af;
    float ar{1.0f};
    bool operator==(const State&) const = default;
};
[[nodiscard]] std::optional<State> DecodeState(const nlohmann::json& packet);
void EncodeState(nlohmann::json& packet, const State& state);
[[nodiscard]] bool Newer(u32 next, u32 previous);

struct Entry {
    u32 aid{}, command_index{};
    bool child{}, loop{}, root_zero_verified{}, phase_known{};
    float frames{};
    std::string command, clip, end_policy;
};

struct BlackboardField {
    u32 type{}, index{};
    std::string name;
};
struct BlackboardKeys {
    u64 instance_off{}, int_off{}, float_off{}, bool_off{}, data_off{};
    u64 resource_off{}, manager_off{}, blob_off{}, table_off{}, pool_off{};
    u64 magic_off{}, version_off{}, type_stride{}, count_off{}, start_off{}, names_off{};
    u32 magic{}, version{};
    std::vector<BlackboardField> fields;
};
struct PlayerKeys {
    std::vector<u64> chain;
    u32 count_stage{}, count_min{};
    u64 count_off{}, name_off{}, link_id_off{}, link_record_off{}, record_actor_off{};
    u64 record_id_off{}, lifecycle_off{};
    u32 lifecycle_lo{}, lifecycle_hi{};
    std::vector<u64> candidates;
    std::array<std::string, 2> names; // adult, child
};
struct Keys {
    bool sender_enabled{}, receiver_enabled{}, restart_frame_reset{}, endpoint_inclusive{};
    std::vector<u64> ctl_chain;
    u64 ctl_count_off{}, ctl_entries_off{}, ctl_stride{};
    u64 cmd_index_off{}, cmd_name_off{}, frame_off{}, rate_off{};
    u32 source_slot{};
    OotGameDataTrace::Entry play_wrapper, frame_setter, rate_setter;
    std::array<std::vector<std::string>, 2> cmd_table; // adult, child
    std::array<std::map<u32, u32>, 2> representatives;
    std::array<std::array<u32, 2>, 2> move_ids; // adult/child, walk/run
    std::string move_command;
    std::set<u32> phase_known_ids;
    std::optional<BlackboardKeys> blackboard;
    std::optional<PlayerKeys> player;
};
[[nodiscard]] std::optional<Keys> DecodeKeys(const nlohmann::json& value);

class Table {
public:
    [[nodiscard]] static std::shared_ptr<const Table> Parse(std::string_view bytes,
                                                          std::string* why = nullptr);
    [[nodiscard]] const Entry* Find(bool child, u32 aid) const;
    [[nodiscard]] const Entry* Map(bool child, u32 command_index, std::string_view command,
                                   u32 legacy_category, const Keys& keys) const;
    [[nodiscard]] const std::string& Hash() const { return hash; }
    [[nodiscard]] size_t Size() const { return entries.size(); }
private:
    std::map<u32, Entry> entries;
    std::array<std::map<u32, std::vector<u32>>, 2> commands;
    std::string hash;
};
[[nodiscard]] std::shared_ptr<const Table> LoadTable(std::string* why = nullptr);
[[nodiscard]] bool ValidHash(std::string_view hash);

struct Controller {
    u64 address{};
    u32 index{};
    std::string name;
    float frame{}, rate{};
    bool operator==(const Controller&) const = default;
};
struct BlackboardValue {
    BlackboardField field;
    double value{};
};
struct Snapshot {
    u64 actor{}, instance{};
    std::vector<Controller> controllers;
    std::vector<BlackboardValue> blackboard;
};
struct Player {
    u64 actor{}, link{}, record{};
    u32 generation{};
    bool child{};
    bool operator==(const Player&) const = default;
};

// Checked reads are shared by the sender snapshot and the ghost's end-of-clip observation.
template <typename Memory>
class Reader {
public:
    explicit Reader(Memory& memory_) : memory{memory_} {}
    bool Range(u64 address, u64 size) const {
        return address && size && address <= std::numeric_limits<u64>::max() - size &&
               memory.IsValidVirtualAddressRange(address, size);
    }
    std::optional<u64> Int(u64 base, u64 offset, size_t size) const {
        if (offset > std::numeric_limits<u64>::max() - base) return std::nullopt;
        const u64 address = base + offset;
        if (!Range(address, size)) return std::nullopt;
        switch (size) {
        case 1: return memory.Read8(address);
        case 2: return memory.Read16(address);
        case 4: return memory.Read32(address);
        case 8: return memory.Read64(address);
        default: return std::nullopt;
        }
    }
    u64 Ptr(u64 base, u64 offset) const { return base ? Int(base, offset, 8).value_or(0) : 0; }
    std::optional<float> Float(u64 base, u64 offset) const {
        const auto bits = Int(base, offset, 4);
        if (!bits) return std::nullopt;
        const float f = std::bit_cast<float>(static_cast<u32>(*bits));
        return std::isfinite(f) ? std::optional<float>{f} : std::nullopt;
    }
    std::optional<std::string> String(u64 address) const {
        std::string out;
        for (u64 i = 0; i < 64; ++i) {
            const auto c = Int(address, i, 1);
            if (!c) return std::nullopt;
            if (!*c) return out.empty() ? std::nullopt : std::optional<std::string>{out};
            if (*c < 32 || *c > 126) return std::nullopt;
            out.push_back(static_cast<char>(*c));
        }
        return std::nullopt;
    }
    u64 Chain(u64 base, const std::vector<u64>& chain) const {
        for (const auto offset : chain) base = Ptr(base, offset);
        return base;
    }
private:
    Memory& memory;
};

template <typename Memory>
bool ValidatePlayer(Memory& memory, const Player& player, const Keys& keys) {
    if (!keys.player || !player.actor || !player.link || !player.record) return false;
    const auto& k = *keys.player;
    const Reader r{memory};
    const auto generation = r.Int(player.link, k.link_id_off, 4);
    const auto life = r.Int(player.actor, k.lifecycle_off, 4);
    return generation && *generation == player.generation && *generation != 0xFFFFFFFF &&
           r.Ptr(player.link, k.link_record_off) == player.record &&
           r.Int(player.record, k.record_id_off, 4) == generation &&
           r.Ptr(player.record, k.record_actor_off) == player.actor && life &&
           (*life < k.lifecycle_lo || *life > k.lifecycle_hi) &&
           r.String(r.Ptr(player.link, k.name_off)) == k.names[player.child];
}

template <typename Memory>
std::optional<Player> FindPlayer(Memory& memory, u64 base, bool child, const Keys& keys) {
    if (!keys.player) return std::nullopt;
    const auto& k = *keys.player;
    const Reader r{memory};
    for (size_t stage = 0; stage < k.chain.size(); ++stage) {
        base = r.Ptr(base, k.chain[stage]);
        if (!base) return std::nullopt;
        if (stage == k.count_stage) {
            const auto count = r.Int(base, k.count_off, 4);
            if (!count || *count < k.count_min || *count > 4096) return std::nullopt;
        }
    }
    for (const auto offset : k.candidates) {
        Player player;
        player.child = child;
        player.link = r.Ptr(base, offset);
        player.record = r.Ptr(player.link, k.link_record_off);
        player.actor = r.Ptr(player.record, k.record_actor_off);
        const auto generation = r.Int(player.link, k.link_id_off, 4);
        if (!generation) continue;
        player.generation = static_cast<u32>(*generation);
        if (ValidatePlayer(memory, player, keys)) return player;
    }
    return std::nullopt;
}

template <typename Memory>
std::optional<std::vector<BlackboardValue>> ReadBlackboard(Memory& memory, u64 instance,
                                                         const BlackboardKeys& k) {
    const Reader r{memory};
    const u64 board = r.Ptr(instance, k.instance_off);
    const u64 resource = r.Ptr(board, k.resource_off);
    const u64 manager = r.Ptr(resource, k.manager_off);
    const u64 blob = r.Ptr(manager, k.blob_off), table = r.Ptr(manager, k.table_off);
    if (!board || !blob || !table || r.Int(blob, k.magic_off, 4) != k.magic ||
        r.Int(blob, k.version_off, 4) != k.version) return std::nullopt;
    const auto pool_offset = r.Int(blob, k.pool_off, 4);
    if (!pool_offset || *pool_offset > 0x10000000 ||
        blob > std::numeric_limits<u64>::max() - *pool_offset) return std::nullopt;
    const u64 pool = blob + *pool_offset;
    std::vector<BlackboardValue> values;
    for (const auto& field : k.fields) {
        const auto count = r.Int(table, field.type * k.type_stride + k.count_off, 2);
        const auto start = r.Int(table, field.type * k.type_stride + k.start_off, 2);
        if (!count || !start || field.index >= *count || *count > 4096 || *start > 4096)
            return std::nullopt;
        const auto name_offset = r.Int(table, k.names_off + 4 * (*start + field.index), 4);
        if (!name_offset || r.String(pool + (*name_offset & 0xFFFFFF)) != field.name)
            return std::nullopt;
        const u64 array = r.Ptr(board, field.type == 1 ? k.int_off :
                                      field.type == 3 ? k.float_off : k.bool_off);
        const auto size = r.Int(array, 0, 4);
        if (!size || field.index >= *size || *size > 4096) return std::nullopt;
        double value{};
        if (field.type == 3) {
            const auto f = r.Float(array, k.data_off + 4 * u64{field.index});
            if (!f) return std::nullopt;
            value = *f;
        } else {
            const auto bits = r.Int(array, k.data_off + (field.type == 4 ? 1 : 4) *
                                                        u64{field.index}, field.type == 4 ? 1 : 4);
            if (!bits || (field.type == 4 && *bits > 1)) return std::nullopt;
            value = field.type == 4 ? static_cast<double>(*bits) :
                                    static_cast<double>(static_cast<s32>(*bits));
        }
        values.push_back({field, value});
    }
    if (r.Ptr(instance, k.instance_off) != board || r.Ptr(board, k.resource_off) != resource ||
        r.Ptr(resource, k.manager_off) != manager || r.Ptr(manager, k.blob_off) != blob ||
        r.Ptr(manager, k.table_off) != table) return std::nullopt;
    return values;
}

template <typename Memory>
std::optional<float> ReadControllerFrame(Memory& memory, u64 actor, const Keys& keys,
                                         u32 slot = 0) {
    const Reader r{memory};
    const u64 instance = r.Chain(actor, keys.ctl_chain);
    const auto count = r.Int(instance, keys.ctl_count_off, 4);
    const u64 entries = r.Ptr(instance, keys.ctl_entries_off);
    if (!count || *count > 64 || slot >= *count || !keys.ctl_stride) return std::nullopt;
    const u64 controller = r.Ptr(entries, keys.ctl_stride * slot);
    const auto frame = r.Float(controller, keys.frame_off);
    if (!controller || !frame || *frame < 0 || *frame > 1000000 ||
        r.Chain(actor, keys.ctl_chain) != instance ||
        r.Ptr(instance, keys.ctl_entries_off) != entries ||
        r.Ptr(entries, keys.ctl_stride * slot) != controller) return std::nullopt;
    return frame;
}

template <typename Memory, typename Validate>
std::optional<Snapshot> ReadSnapshot(Memory& memory, u64 actor, bool child, const Keys& keys,
                                     Validate validate) {
    if (!keys.sender_enabled || keys.ctl_chain.empty() || !validate()) return std::nullopt;
    const Reader r{memory};
    Snapshot snapshot;
    snapshot.actor = actor;
    snapshot.instance = r.Chain(actor, keys.ctl_chain);
    const auto count = r.Int(snapshot.instance, keys.ctl_count_off, 4);
    const u64 entries = r.Ptr(snapshot.instance, keys.ctl_entries_off);
    if (!count || !*count || *count > 64 || keys.source_slot >= *count || !keys.ctl_stride)
        return std::nullopt;
    for (u32 slot = 0; slot < *count; ++slot) {
        Controller controller;
        controller.address = r.Ptr(entries, keys.ctl_stride * slot);
        const auto index = r.Int(controller.address, keys.cmd_index_off, 2);
        const auto name = r.String(r.Ptr(controller.address, keys.cmd_name_off));
        const auto frame = r.Float(controller.address, keys.frame_off);
        const auto rate = r.Float(controller.address, keys.rate_off);
        if (!index || *index >= CommandLimit || *index >= keys.cmd_table[child].size() ||
            !name || keys.cmd_table[child][*index] != *name || !frame || *frame < 0 ||
            *frame > 1000000 || !rate || *rate < 0 || *rate > 4) {
            // Only the source slot must hold a normal command; another slot (a partial
            // controller with no command, a fallback sentinel) is recorded as unknown.
            if (slot == keys.source_slot || !controller.address) return std::nullopt;
            controller.index = UnknownCommand;
            snapshot.controllers.push_back(std::move(controller));
            continue;
        }
        controller.index = static_cast<u32>(*index);
        controller.name = *name;
        controller.frame = *frame;
        controller.rate = *rate;
        snapshot.controllers.push_back(std::move(controller));
    }
    if (keys.blackboard) {
        auto values = ReadBlackboard(memory, snapshot.instance, *keys.blackboard);
        if (!values) return std::nullopt;
        snapshot.blackboard = std::move(*values);
    }
    if (!validate() || r.Chain(actor, keys.ctl_chain) != snapshot.instance ||
        r.Int(snapshot.instance, keys.ctl_count_off, 4) != count ||
        r.Ptr(snapshot.instance, keys.ctl_entries_off) != entries) return std::nullopt;
    for (size_t slot = 0; slot < snapshot.controllers.size(); ++slot) {
        const auto& controller = snapshot.controllers[slot];
        if (r.Ptr(entries, keys.ctl_stride * slot) != controller.address) return std::nullopt;
        if (controller.index == UnknownCommand) continue;
        if (r.Int(controller.address, keys.cmd_index_off, 2) != controller.index ||
            r.String(r.Ptr(controller.address, keys.cmd_name_off)) != controller.name)
            return std::nullopt;
    }
    return snapshot;
}

class Sender {
public:
    [[nodiscard]] State Sample(const Table& table, const Keys& keys, const Snapshot& snapshot,
                                bool child, u32 legacy_category,
                                std::optional<u64> successful_start = std::nullopt);
    void Reset();
private:
    State last;
    std::optional<Controller> previous;
    std::optional<u64> start;
    u64 actor{};
    bool age{};
};

struct Lifetime {
    std::string session, scene;
    u64 actor{}, generation{}, epoch{};
    bool child{};
    bool operator==(const Lifetime&) const = default;
};
enum class Stage { Play, Frame, Rate, Anchor };
struct Operation {
    Stage stage{};
    State state;
    Entry entry;
    Lifetime lifetime;
    u64 serial{};
};
class Receiver {
public:
    // The owner validates scene, resource readiness and actor identity before this call.
    bool Queue(const State& state, u32 tick, const Lifetime& lifetime, const Table& table,
               bool hash_matches);
    [[nodiscard]] std::optional<Operation> Next(u64 now_ms, const Lifetime& lifetime);
    bool Complete(const Operation& operation, bool success, u64 now_ms,
                  const Lifetime& lifetime);
    void ObserveFrame(float frame, bool endpoint_inclusive = false);
    void Reset();
    [[nodiscard]] const std::optional<State>& Committed() const { return committed; }
    [[nodiscard]] bool Busy() const { return in_flight.has_value(); }
    /// The current transition failed its bounded retries: the owner draws the legacy category
    /// until the sender starts another transition (which clears this).
    [[nodiscard]] bool Exhausted() const {
        return current && !in_flight && retries >= RetryLimit &&
               (!pending || SameTransition(pending->state, current->state));
    }
    static constexpr u32 RetryLimit = 3;
    static constexpr u64 RetryMs = 250;
private:
    struct Target { State state; Entry entry; };
    std::optional<Lifetime> owner;
    std::optional<u32> tick;
    std::optional<Target> pending, current;
    std::optional<State> committed;
    std::optional<Operation> in_flight;
    Stage stage{Stage::Play};
    u64 serial{}, retry_at{};
    u32 retries{};
    bool holding{};
    std::optional<float> observed; ///< Last controller frame seen for the current transition
    static bool SameTransition(const State& a, const State& b) {
        return a.aid == b.aid && a.aseq == b.aseq;
    }
};

[[nodiscard]] std::array<u8, 0x30> BuildPlayRequest(u64 actor, u64 c_string);

} // namespace Core::OotCoop::Anim
