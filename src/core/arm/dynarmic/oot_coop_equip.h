// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "core/arm/dynarmic/oot_gamedata_trace.h"

namespace Core::OotCoop::Equip {
struct Appearance {
    u8 sword{7}, shield{7}, sword_state{3}, shield_state{3}, tunic{3}, boots{3}, mask{15}, age{3}, guard{};
    bool operator==(const Appearance&) const = default;
};
std::optional<u32> Pack(const Appearance& value);
std::optional<Appearance> Unpack(u64 word, std::optional<bool> child = {});
bool Settled(u8 id, u8 state);
bool Newer(u16 value, u16 previous);
class Sequence {
public:
    u16 Sample(const Appearance& value);
    void Reset() { *this = {}; }
private:
    Appearance known;
    u16 sequence{};
};
struct Packet {
    u32 word{};
    u16 sequence{};
    bool operator==(const Packet&) const = default;
};
std::optional<Packet> DecodePacket(const nlohmann::json& value, bool child);

enum class Slot : size_t { Sword, Shield, Tunic, Boots, Head, Sheath, Count };
inline constexpr size_t SlotCount = static_cast<size_t>(Slot::Count);
struct SlotKeys { u64 link{}, handle{}; u32 create{}, dynamic{}, request_mask{}; };
struct BootGuard {
    u64 at{}, resume{};
    u32 equipment_register{}, slot_register{};
    std::vector<u32> words;
    bool Valid() const;
};
u32 CompareFlags(u32 left, u32 right, u32 pstate);
struct Keys {
    std::map<std::string, u64, std::less<>> fields;
    std::array<SlotKeys, SlotCount> slots;
    OotGameDataTrace::Entry dynamic, statik, draw, sheathe, state, select_player;
    bool enabled{}, boots_enabled{}, recreate_none{};
    BootGuard guard;
    std::array<u64, 5> dynamic_arguments{}, static_arguments{};
    u64 Get(std::string_view field) const { return fields.at(std::string{field}); }
};
std::shared_ptr<const Keys> DecodeKeys(const nlohmann::json& value);
struct Name {
    Slot slot{};
    u8 id{}, age{3};
    std::string source, target, sheath;
};
struct Names {
    std::array<std::string, 2> players;
    std::vector<Name> rows;
    const Name* Find(Slot slot, u8 id, u8 age) const;
    u8 Identify(Slot slot, std::string_view name, u8 age) const;
};
std::optional<Names> DecodeNames(const nlohmann::json& document);
std::optional<Names> LoadNames(std::string_view build);

// All memory access stays on the existing guest frame lane; Call uses its full-context applier.
struct Port {
    std::function<std::optional<u8>(u64)> r8;
    std::function<std::optional<u32>(u64)> r32;
    std::function<std::optional<u64>(u64)> r64;
    std::function<bool(u64, const void*, size_t)> write;
    std::function<std::optional<u64>(const OotGameDataTrace::Entry&)> verified;
    std::function<bool(u64, const std::array<u64, 8>&, std::optional<float>)> call;
    std::function<void(std::string_view)> log;
};
struct ActorName {
    std::array<char, 64> bytes{};
    size_t length{};
    ActorName() = default;
    ActorName(std::string_view text) : length{std::min(text.size(), bytes.size() - 1)} {
        std::copy_n(text.data(), length, bytes.data());
    }
    ActorName(const char* text) : ActorName{std::string_view{text}} {}
    ActorName(const std::string& text) : ActorName{std::string_view{text}} {}
    operator std::string_view() const { return {bytes.data(), length}; }
    bool operator==(const ActorName&) const = default;
    bool operator==(std::string_view text) const { return std::string_view{*this} == text; }
    bool operator==(const char* text) const { return std::string_view{*this} == text; }
    bool operator==(const std::string& text) const { return std::string_view{*this} == text; }
};
struct Child {
    u64 link{}, record{}, actor{}, name_pointer{};
    u32 generation{};
    ActorName name;
    bool operator==(const Child&) const = default;
};
std::optional<Child> ReadLink(const Port& port, const Keys& keys, u64 link, const Child* cached = nullptr);
bool VerifyBootGuard(const Port& port, const Keys& keys, u64 base);
bool Related(const Port& port, const Keys& keys, u64 base, u64 owner, u64 child);
struct Token {
    std::string peer, scene;
    u64 epoch{}, actor{}, record{}, equipment{};
    u32 generation{};
    u8 age{};
    bool operator==(const Token&) const = default;
};
struct Root {
    Token token;
    bool eligible{};
    std::optional<Packet> desired;
};
struct Observation {
    std::optional<Child> child;
    u64 handle{};
    u32 phase{}, detail{}, bits{}, errors{};
    bool readable{}, idle{};
    u8 actual{3};
};
enum class Action { None, Create, Query, Draw, Sheathe, Quarantine, Recreate };
struct Work { Action action{}; Slot slot{}; u8 id{}; };
class Reconciler {
public:
    bool Desired(Packet packet, u8 age);
    Work Step(const std::array<Observation, SlotCount>& observations, const Names& names,
              u8 age, bool eligible, bool recreate_none, bool boots_guarded = false);
    void Issued(Work work, const Observation& previous);
    void Quarantine() { quarantined = true; }
    bool Quarantined() const { return quarantined; }
    const Appearance& Desired() const { return desired; }
    bool WantsNone() const;
private:
    struct Request { bool pending{}; u8 id{}; u32 old_generation{0xffffffffU}; u32 updates{}; Action action{}; };
    Appearance desired;
    std::optional<u16> sequence;
    std::array<Request, SlotCount> requests{};
    bool quarantined{};
};
std::array<u64, 8> Arguments(const Keys& keys, Work work, u64 equipment, u64 cell);

// Bounded, never-reused generation scratch, including parked owners with late native work.
inline constexpr u64 ScratchOffset = 0x1000;
inline constexpr u64 TreeBytes = 0x400;
inline constexpr size_t MaxTrees = 32;
inline constexpr u64 PersistentBytes = ScratchOffset + MaxTrees * TreeBytes;
class Controller {
public:
    Controller(std::shared_ptr<const Keys> keys, Names names, OotGameDataTrace::GhostOffsets ghost,
               u64 base, u64 scratch);
    bool Calling() const;
    bool Frame(Port& port, const std::vector<Root>& roots, std::string_view scene, bool eligible);
    void Returned(Port& port, u64 result);
    void CheckContext(const std::vector<Root>& roots, std::string_view scene, bool eligible);
    std::optional<Packet> Local(bool child) const;
    bool Blocked(u64 actor, u32 generation) const;
    bool Recreate(u64 actor, u32 generation) const;
    void Invalidate();
    std::optional<u64> Guard(Port& port, u64 pc, u64 equipment, u32 slot);
private:
    struct Tree {
        Token token;
        Reconciler reconcile;
        std::array<Observation, SlotCount> observed{};
        std::array<std::optional<Child>, SlotCount> queried{};
        std::array<u64, SlotCount> query_ticks{};
        std::vector<Child> retired_children;
        bool retired{}, recreate{}, none_logged{};
        u64 scratch{};
    };
    enum class Call { None, Select, LocalState, Work, Park, Hide };
    struct Pending {
        Call call{};
        size_t tree{}, slot{};
        Token token;
        Work work;
        std::optional<Child> child;
        bool guard_hit{};
        bool accept{true};
    } pending;
    std::shared_ptr<const Keys> keys;
    Names names;
    OotGameDataTrace::GhostOffsets ghost;
    u64 base{}, scratch{}, epoch{1}, tick{}, local_tick{};
    std::string scene;
    std::vector<Tree> trees;
    std::optional<Child> player;
    u64 player_equipment{};
    Appearance local;
    Sequence sequence;
    std::optional<Packet> local_packet;
    size_t local_stage{};
    std::array<std::optional<Child>, SlotCount> local_children{};
    std::array<u32, SlotCount> absent_samples{};
    size_t retire_cursor{}, active_cursor{};
    bool valid(const Port& port, const Token& token) const;
    std::optional<u64> equipment(const Port& port, u64 actor, bool private_owner) const;
    Observation observe(const Port& port, const Token& token, Slot slot, const Child* cached = nullptr) const;
    bool start(Port& port, const OotGameDataTrace::Entry& entry, std::array<u64, 8> args,
               Pending call, std::optional<float> alpha = {});
    bool sender(Port& port);
    void dump(Port& port, const Tree& tree, std::string_view reason) const;
};
} // namespace Core::OotCoop::Equip
