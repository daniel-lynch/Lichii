// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <map>
#include <string>
#include <catch2/catch_test_macros.hpp>
#include "core/arm/dynarmic/oot_coop_equip.h"

namespace {
namespace Equip = Core::OotCoop::Equip;
using Json = nlohmann::json;
using Equip::Action;
using Equip::Slot;
constexpr size_t Index(Slot slot) { return static_cast<size_t>(slot); }

Equip::Packet Packet(Equip::Appearance appearance, u16 sequence = 1) {
    const auto word = Equip::Pack(appearance);
    REQUIRE(word);
    return {*word, sequence};
}

Equip::Names Names() {
    Equip::Names names;
    names.players = {"SyntheticYoung", "SyntheticOld"};
    names.rows = {{Slot::Sword, 1, 3, "SourceA", "PrivateA", "PrivateCaseA"},
                  {Slot::Sword, 2, 3, "SourceB", "PrivateB", "PrivateCaseB"},
                  {Slot::Sword, 3, 3, "SourceC", "PrivateC", "PrivateCaseC"},
                  {Slot::Shield, 1, 3, "SourceD", "PrivateD", {}},
                  {Slot::Tunic, 1, 1, "SourceE", "PrivateE", {}},
                  {Slot::Head, 0, 3, "SourceF", "PrivateF", {}},
                  {Slot::Head, 1, 0, "SourceG", "PrivateG", {}}};
    return names;
}

std::array<Equip::Observation, Equip::SlotCount> Empty() {
    std::array<Equip::Observation, Equip::SlotCount> observations;
    for (auto& observation : observations) {
        observation.readable = true;
        observation.idle = true;
    }
    return observations;
}

void Child(std::array<Equip::Observation, Equip::SlotCount>& observations, Slot slot,
           std::string name, u32 generation = 10, u8 actual = 2) {
    auto& observation = observations[Index(slot)];
    observation.child = Equip::Child{0x1000, 0x2000, 0x3000, 0x4000, generation, std::move(name)};
    observation.actual = actual;
}

struct Memory {
    std::map<u64, u8> bytes;
    void Write(u64 address, u64 value, size_t size) {
        for (size_t i = 0; i < size; ++i) bytes[address + i] = static_cast<u8>(value >> (8 * i));
    }
    std::optional<u64> Read(u64 address, size_t size) const {
        u64 value{};
        for (size_t i = 0; i < size; ++i) {
            const auto at = bytes.find(address + i);
            if (at == bytes.end()) return {};
            value |= u64{at->second} << (8 * i);
        }
        return value;
    }
    void Text(u64 address, std::string_view text) {
        for (size_t i = 0; i < text.size(); ++i) bytes[address + i] = static_cast<u8>(text[i]);
        bytes[address + text.size()] = 0;
    }
    Equip::Port Port() {
        Equip::Port port;
        port.r8 = [this](u64 address) -> std::optional<u8> {
            const auto value = Read(address, 1);
            return value ? std::optional{static_cast<u8>(*value)} : std::nullopt;
        };
        port.r32 = [this](u64 address) -> std::optional<u32> {
            const auto value = Read(address, 4);
            return value ? std::optional{static_cast<u32>(*value)} : std::nullopt;
        };
        port.r64 = [this](u64 address) { return Read(address, 8); };
        return port;
    }
};

Equip::Keys MemoryKeys() {
    Equip::Keys keys;
    keys.fields = {{"link_record", 0}, {"link_id", 8}, {"record_actor", 0}, {"record_id", 8},
                   {"actor_name", 32}, {"lifecycle", 48}, {"lifecycle_lo", 7}, {"lifecycle_hi", 11},
                   {"manager_cell", 64}, {"ring_indices", 80}, {"ring_capacity", 88},
                   {"ring_start", 92}, {"ring_count", 96}, {"pairs", 16}, {"pair_count", 24},
                   {"pair_stride", 32}, {"pair_child", 16}};
    return keys;
}

Memory LinkMemory() {
    Memory memory;
    memory.Write(0x1000, 0x2000, 8);
    memory.Write(0x1008, 42, 4);
    memory.Write(0x2000, 0x3000, 8);
    memory.Write(0x2008, 42, 4);
    memory.Write(0x3020, 0x4000, 8);
    memory.Write(0x3030, 6, 4);
    memory.Text(0x4000, "SyntheticChild");
    return memory;
}

Equip::BootGuard BootGuard() {
    // Synthetic CMP W5,#23; B.NE +12; NOP; NOP. No native game instructions or offsets.
    return {0xa000, 0xa010, 4, 5,
            {0x7100001fU | (23U << 10) | (5U << 5), 0x54000061U, 0xd503201fU, 0xd503201fU}};
}

struct ControllerFixture {
    static constexpr u64 Base = 0x1000, Actor = 0x10000, Record = 0x11000,
                         Equipment = 0x12000, Holder = 0x13000, Param = 0x14000,
                         NameAt = 0x15000, Model = 0x16000, Scratch = 0x80000;
    static constexpr u64 CreateEntry = 0x9000, ParkEntry = 0x9010, HideEntry = 0x9020;
    struct Call {
        u64 entry{};
        std::array<u64, 8> args{};
        std::optional<float> alpha;
    };
    Memory memory;
    std::shared_ptr<Equip::Keys> keys = std::make_shared<Equip::Keys>();
    Equip::Port port;
    Equip::Root root;
    std::vector<Call> calls;
    std::vector<std::string> logs;
    std::unique_ptr<Equip::Controller> controller;

    explicit ControllerFixture(bool guarded_boots = false) {
        *keys = MemoryKeys();
        keys->enabled = true;
        keys->fields.insert({{"holder", 0x100}, {"equipment", 0x20}, {"owner", 0x28},
                             {"param", 0x30}, {"pouch", 0}, {"sheath_user", 1},
                             {"actor_record", 0x110}, {"actor_id", 0x118},
                             {"model", 0x18}, {"bookkeeping", 0x200}, {"bookkeeping_stride", 8},
                             {"request_bits", 0x700}, {"error_flags", 0x704}, {"error_mask", 1},
                             {"player_system", 0x800}});
        keys->fields["actor_name"] = 0x120;
        keys->fields["lifecycle"] = 0x130;
        keys->dynamic.offset = CreateEntry;
        keys->statik.offset = CreateEntry + 4;
        keys->state.offset = CreateEntry + 8;
        for (size_t i = 0; i < Equip::SlotCount; ++i) {
            auto& slot = keys->slots[i];
            slot = {0x400 + i * 32, 0x600 + i * 8, static_cast<u32>(20 + i),
                    static_cast<u32>(10 + i), 1U << i};
            memory.Write(Equipment + slot.link, 0, 8);
            memory.Write(Equipment + slot.link + 8, 0xffffffff, 4);
            memory.Write(Equipment + slot.handle, 0, 8);
            memory.Write(Equipment + 0x200 + slot.create * 8, 0, 8);
        }
        memory.Write(Equipment + 0x700, 0, 4);
        memory.Write(Equipment + 0x704, 0, 4);
        memory.Write(Actor + 0x100, Holder, 8);
        memory.Write(Actor + 0x110, Record, 8);
        memory.Write(Actor + 0x118, 10, 4);
        memory.Write(Actor + 0x120, NameAt, 8);
        memory.Write(Actor + 0x130, 6, 4);
        memory.Write(Record, Actor, 8);
        memory.Write(Record + 8, 10, 4);
        memory.Write(Holder + 0x20, Equipment, 8);
        memory.Write(Holder + 0x18, Model, 8);
        memory.Write(Equipment + 0x28, Actor, 8);
        memory.Write(Equipment + 0x30, Param, 8);
        memory.Write(Param, 0, 1);
        memory.Write(Param + 1, 1, 1);
        memory.Text(NameAt, "PrivateOwner");
        port = memory.Port();
        port.write = [this](u64 at, const void* bytes, size_t size) {
            if (at < Scratch || at + size > Scratch + Equip::MaxTrees * Equip::TreeBytes) return false;
            const auto* data = static_cast<const u8*>(bytes);
            for (size_t i = 0; i < size; ++i) memory.bytes[at + i] = data[i];
            return true;
        };
        port.verified = [](const Core::OotGameDataTrace::Entry& entry) -> std::optional<u64> {
            return entry.offset ? std::optional{entry.offset} : std::nullopt;
        };
        port.call = [this](u64 entry, const std::array<u64, 8>& args, std::optional<float> alpha) {
            calls.push_back({entry, args, alpha});
            return true;
        };
        port.log = [this](std::string_view line) { logs.emplace_back(line); };
        Core::OotGameDataTrace::GhostOffsets ghost;
        ghost.actor_child = "PrivateOwner";
        ghost.actor_adult = "PrivateOwner";
        ghost.set_mtx.offset = ParkEntry;
        ghost.alpha_set.offset = HideEntry;
        ghost.alpha_set.words = {0x12345678};
        Equip::Appearance desired;
        desired.sword = 1;
        desired.sword_state = 2;
        desired.age = 0;
        root = {{"peerA", "SceneA", 1, Actor, Record, Equipment, 10, 0}, true, Packet(desired)};
        auto names = Names();
        if (guarded_boots) {
            keys->boots_enabled = true;
            keys->guard = BootGuard();
            for (size_t i = 0; i < keys->guard.words.size(); ++i)
                memory.Write(Base + keys->guard.at + i * 4, keys->guard.words[i], 4);
            names.rows.push_back({Slot::Boots, 1, 1, "SourceBoots", "PrivateBoots", {}});
            desired = {};
            desired.age = 1;
            desired.boots = 1;
            root.token.age = 1;
            root.desired = Packet(desired);
        }
        controller = std::make_unique<Equip::Controller>(keys, std::move(names), ghost, Base, Scratch);
    }

    bool Frame() { return controller->Frame(port, {root}, root.token.scene, true); }
    void UntilCall() {
        for (unsigned i = 0; i < 32; ++i) if (Frame()) return;
        FAIL("expected a bounded equipment call");
    }
};
} // namespace

TEST_CASE("Equipment word exhausts each mod field", "[oot][coop][equip]") {
    constexpr std::array members{&Equip::Appearance::sword, &Equip::Appearance::shield,
        &Equip::Appearance::sword_state, &Equip::Appearance::shield_state,
        &Equip::Appearance::tunic, &Equip::Appearance::boots, &Equip::Appearance::mask,
        &Equip::Appearance::age, &Equip::Appearance::guard};
    constexpr std::array<unsigned, 9> shifts{0, 3, 6, 8, 10, 12, 14, 18, 20};
    constexpr std::array<unsigned, 9> widths{3, 3, 2, 2, 2, 2, 4, 2, 1};
    for (size_t field = 0; field < members.size(); ++field) {
        for (unsigned value = 0; value < (1U << widths[field]); ++value) {
            CAPTURE(field, value);
            Equip::Appearance appearance{0, 0, 0, 0, 0, 0, 0, 0, 0};
            appearance.*members[field] = static_cast<u8>(value);
            const bool allowed = field < 2 ? value <= 3 || value == 7 :
                                 field == 6 ? value <= 10 || value == 15 : field != 7 || value != 2;
            const auto word = (1U << 28) | (value << shifts[field]);
            REQUIRE(Equip::Pack(appearance).has_value() == allowed);
            REQUIRE(Equip::Unpack(word).has_value() == allowed);
            if (allowed) {
                REQUIRE(Equip::Pack(appearance) == word);
                REQUIRE(Equip::Unpack(word) == appearance);
            }
        }
        Equip::Appearance overflow;
        overflow.*members[field] = static_cast<u8>(1U << widths[field]);
        REQUIRE_FALSE(Equip::Pack(overflow));
    }
}

TEST_CASE("Equipment rejects malformed words and contradictory age", "[oot][coop][equip]") {
    for (u64 word : {~u64{}, u64{1} << 32, u64{0}, u64{0x10200000}, u64{0x10080000},
                     u64{0x10000004}, u64{0x1002c000}, u64{0x20000000}}) {
        CAPTURE(word);
        REQUIRE_FALSE(Equip::Unpack(word));
    }
    for (unsigned bit = 21; bit < 28; ++bit) REQUIRE_FALSE(Equip::Unpack(0x10000000U | (1U << bit)));
    REQUIRE(Equip::Unpack(0x10000000, true));
    REQUIRE_FALSE(Equip::Unpack(0x10000000, false));
    REQUIRE(Equip::Unpack(0x10040000, false));
    REQUIRE_FALSE(Equip::Unpack(0x10040000, true));
    REQUIRE(Equip::Unpack(0x100c0000, true));
    REQUIRE(Equip::Unpack(0x100c0000, false));
    REQUIRE(Equip::Settled(0, 0));
    REQUIRE(Equip::Settled(1, 1));
    REQUIRE(Equip::Settled(3, 2));
    REQUIRE_FALSE(Equip::Settled(0, 1));
    REQUIRE_FALSE(Equip::Settled(1, 0));
    REQUIRE_FALSE(Equip::Settled(7, 2));
    REQUIRE_FALSE(Equip::Settled(1, 3));
}

TEST_CASE("Equipment packet requires actual JSON integers", "[oot][coop][equip]") {
    const Json good{{"eq32", 0x10000000}, {"eqseq", 65535}};
    REQUIRE(Equip::DecodePacket(good, true) == Equip::Packet{0x10000000, 65535});
    REQUIRE_FALSE(Equip::DecodePacket(good, false));
    for (const auto& field : {"eq32", "eqseq"}) {
        for (const Json& bad : {Json(true), Json(false), Json(-1), Json(1.0), Json("0x10000000"), Json(nullptr)}) {
            auto packet = good;
            packet[field] = bad;
            REQUIRE_FALSE(Equip::DecodePacket(packet, true));
        }
        auto missing = good;
        missing.erase(field);
        REQUIRE_FALSE(Equip::DecodePacket(missing, true));
    }
    REQUIRE_FALSE(Equip::DecodePacket(Json{{"eq32", u64{1} << 32}, {"eqseq", 0}}, true));
    REQUIRE_FALSE(Equip::DecodePacket(Json{{"eq32", 0x10000000}, {"eqseq", 65536}}, true));
}

TEST_CASE("Equipment sequence tracks known settled changes and wraps", "[oot][coop][equip]") {
    Equip::Sequence sequence;
    Equip::Appearance appearance;
    REQUIRE(sequence.Sample(appearance) == 0);
    appearance.sword = 1;
    appearance.sword_state = 2;
    REQUIRE(sequence.Sample(appearance) == 1);
    appearance.guard = 1;
    REQUIRE(sequence.Sample(appearance) == 1);
    REQUIRE(sequence.Sample({}) == 1);
    appearance.sword = 2;
    appearance.sword_state = 0;
    REQUIRE(sequence.Sample(appearance) == 1);
    appearance.sword_state = 1;
    REQUIRE(sequence.Sample(appearance) == 2);
    appearance.age = 0;
    REQUIRE(sequence.Sample(appearance) == 3);
    sequence.Reset();
    for (u32 i = 0; i < 65536; ++i) {
        appearance.tunic = static_cast<u8>(i % 2);
        REQUIRE(sequence.Sample(appearance) == static_cast<u16>(i + 1));
    }
    REQUIRE(Equip::Newer(0, 65535));
    REQUIRE_FALSE(Equip::Newer(65535, 0));
    REQUIRE_FALSE(Equip::Newer(10, 10));
    REQUIRE_FALSE(Equip::Newer(32768, 0));
    REQUIRE(Equip::Newer(32767, 0));
}

TEST_CASE("Equipment reconciliation waits for sword and private sheath before other slots", "[oot][coop][equip]") {
    Equip::Reconciler reconcile;
    Equip::Appearance appearance{1, 1, 1, 1, 1, 3, 0, 1, 0};
    REQUIRE(reconcile.Desired(Packet(appearance), 1));
    auto observations = Empty();
    const auto names = Names();
    REQUIRE(reconcile.Step(observations, names, 1, false, false).action == Action::None);
    auto work = reconcile.Step(observations, names, 1, true, false);
    REQUIRE(work.action == Action::Create);
    REQUIRE(work.slot == Slot::Sword);
    reconcile.Issued(work, observations[0]);
    Child(observations, Slot::Sword, "PrivateA");
    REQUIRE(reconcile.Step(observations, names, 1, true, false).action == Action::None);
    Child(observations, Slot::Sheath, "WrongCase");
    REQUIRE(reconcile.Step(observations, names, 1, true, false).action == Action::None);
    Child(observations, Slot::Sheath, "PrivateCaseA");
    for (const auto slot : {Slot::Shield, Slot::Tunic, Slot::Head}) {
        work = reconcile.Step(observations, names, 1, true, false);
        REQUIRE(work.action == Action::Create);
        REQUIRE(work.slot == slot);
        reconcile.Issued(work, observations[Index(slot)]);
        Child(observations, slot, names.Find(slot, slot == Slot::Head ? 0 : 1, 1)->target);
    }
    work = reconcile.Step(observations, names, 1, true, false);
    REQUIRE(work.action == Action::Draw);
    REQUIRE(work.slot == Slot::Sword);
    reconcile.Issued(work, observations[0]);
    observations[0].actual = 1;
    work = reconcile.Step(observations, names, 1, true, false);
    REQUIRE(work.action == Action::Draw);
    REQUIRE(work.slot == Slot::Shield);
}

TEST_CASE("Equipment replacements coalesce behind their predecessor generation", "[oot][coop][equip]") {
    Equip::Reconciler reconcile;
    Equip::Appearance appearance;
    appearance.age = 0;
    appearance.sword = 2;
    appearance.sword_state = 2;
    REQUIRE(reconcile.Desired(Packet(appearance, 65535), 0));
    auto observations = Empty();
    const auto names = Names();
    Child(observations, Slot::Sword, "PrivateA", 10);
    Child(observations, Slot::Sheath, "PrivateCaseA");
    auto work = reconcile.Step(observations, names, 0, true, false);
    REQUIRE(work.id == 2);
    reconcile.Issued(work, observations[0]);
    appearance.sword = 3;
    REQUIRE(reconcile.Desired(Packet(appearance, 0), 0));
    REQUIRE_FALSE(reconcile.Desired(Packet(appearance, 65534), 0));
    Child(observations, Slot::Sword, "PrivateB", 10);
    Child(observations, Slot::Sheath, "PrivateCaseB");
    REQUIRE(reconcile.Step(observations, names, 0, true, false).action == Action::None);
    observations[0].child->generation = 11;
    work = reconcile.Step(observations, names, 0, true, false);
    REQUIRE(work.action == Action::Create);
    REQUIRE(work.id == 3);
    REQUIRE(work.slot == Slot::Sword);
    appearance.sword = 7;
    appearance.sword_state = 3;
    REQUIRE(reconcile.Desired(Packet(appearance, 1), 0));
    REQUIRE(reconcile.Desired().sword == 3);
    appearance.age = 3;
    REQUIRE_FALSE(reconcile.Desired(Packet(appearance, 2), 0));
}

TEST_CASE("Equipment cannot skip an unreadable or busy earlier slot", "[oot][coop][equip]") {
    Equip::Reconciler reconcile;
    Equip::Appearance appearance{1, 1, 2, 2, 3, 3, 15, 0, 0};
    REQUIRE(reconcile.Desired(Packet(appearance), 0));
    auto observations = Empty();
    observations[0].idle = false;
    REQUIRE(reconcile.Step(observations, Names(), 0, true, false).action == Action::None);
    observations[0].idle = true;
    observations[0].readable = false;
    REQUIRE(reconcile.Step(observations, Names(), 0, true, false).action == Action::None);
}

TEST_CASE("Equipment timeout counts eligible updates and latches quarantine", "[oot][coop][equip]") {
    Equip::Reconciler reconcile;
    Equip::Appearance appearance;
    appearance.age = 0;
    appearance.sword = 1;
    appearance.sword_state = 2;
    REQUIRE(reconcile.Desired(Packet(appearance), 0));
    auto observations = Empty();
    const auto names = Names();
    const auto work = reconcile.Step(observations, names, 0, true, false);
    reconcile.Issued(work, observations[0]);
    for (unsigned i = 0; i < 400; ++i)
        REQUIRE(reconcile.Step(observations, names, 0, false, false).action == Action::None);
    for (unsigned i = 0; i < 299; ++i)
        REQUIRE(reconcile.Step(observations, names, 0, true, false).action == Action::None);
    REQUIRE(reconcile.Step(observations, names, 0, true, false).action == Action::Quarantine);
    REQUIRE(reconcile.Quarantined());
    Child(observations, Slot::Sword, "PrivateA");
    Child(observations, Slot::Sheath, "PrivateCaseA");
    REQUIRE(reconcile.Step(observations, names, 0, true, false).action == Action::Quarantine);
}

TEST_CASE("Equipment none defaults to preservation and boots stay blocked", "[oot][coop][equip]") {
    Equip::Reconciler reconcile;
    Equip::Appearance appearance;
    appearance.age = 0;
    appearance.sword = 0;
    appearance.sword_state = 0;
    appearance.boots = 1;
    REQUIRE(reconcile.Desired(Packet(appearance), 0));
    auto observations = Empty();
    Child(observations, Slot::Sword, "PrivateA");
    REQUIRE(reconcile.WantsNone());
    auto names = Names();
    names.rows.push_back({Slot::Boots, 1, 3, "SourceBoots", "PrivateBoots", {}});
    REQUIRE(reconcile.Step(observations, names, 0, true, false).action == Action::None);
    REQUIRE(reconcile.Step(observations, names, 0, true, true).action == Action::Recreate);
}

TEST_CASE("Equipment native arguments keep creation and dynamic slot domains separate", "[oot][coop][equip]") {
    Equip::Keys keys;
    keys.dynamic_arguments = {2, 0xffffffff, 1, 0, 0};
    keys.static_arguments = {0, 1, 0, 0, 0};
    for (size_t i = 0; i < Equip::SlotCount; ++i) {
        keys.slots[i].create = static_cast<u32>(20 + i);
        keys.slots[i].dynamic = static_cast<u32>(10 + i);
    }
    constexpr u64 equipment = 0x123000, cell = 0x456000;
    for (const auto slot : {Slot::Sword, Slot::Shield}) {
        const std::array<u64, 8> expected{equipment, keys.slots[Index(slot)].create,
                                         cell, 2, 0xffffffff, 1, 0, 0};
        REQUIRE(Equip::Arguments(keys, {Action::Create, slot, 1}, equipment, cell) == expected);
    }
    for (const auto slot : {Slot::Tunic, Slot::Head}) {
        const std::array<u64, 8> expected{equipment, keys.slots[Index(slot)].create, cell, 0, 1, 0, 0, 0};
        REQUIRE(Equip::Arguments(keys, {Action::Create, slot, 1}, equipment, cell) == expected);
    }
    for (const auto action : {Action::Query, Action::Draw, Action::Sheathe}) {
        const std::array<u64, 8> expected{equipment, keys.slots[Index(Slot::Shield)].dynamic};
        REQUIRE(Equip::Arguments(keys, {action, Slot::Shield, 1}, equipment, cell) == expected);
    }
}

TEST_CASE("Equipment child links reject stale records and retiring lifecycle", "[oot][coop][equip]") {
    auto memory = LinkMemory();
    auto port = memory.Port();
    const auto keys = MemoryKeys();
    const auto child = Equip::ReadLink(port, keys, 0x1000);
    REQUIRE(child);
    REQUIRE(child->generation == 42);
    REQUIRE(child->actor == 0x3000);
    REQUIRE(child->name == "SyntheticChild");
    for (u32 lifecycle = 7; lifecycle <= 11; ++lifecycle) {
        memory.Write(0x3030, lifecycle, 4);
        REQUIRE_FALSE(Equip::ReadLink(port, keys, 0x1000));
    }
    memory.Write(0x3030, 6, 4);
    memory.Write(0x2008, 43, 4);
    REQUIRE_FALSE(Equip::ReadLink(port, keys, 0x1000));
    memory.Write(0x2008, 42, 4);
    memory.Write(0x1008, 0xffffffff, 4);
    REQUIRE_FALSE(Equip::ReadLink(port, keys, 0x1000));
    memory.Write(0x1008, 42, 4);
    memory.Write(0x2000, 0, 8);
    REQUIRE_FALSE(Equip::ReadLink(port, keys, 0x1000));
    REQUIRE_FALSE(Equip::ReadLink(port, keys, 0));
}

TEST_CASE("Equipment links revalidate generation after reading the child", "[oot][coop][equip]") {
    auto memory = LinkMemory();
    auto port = memory.Port();
    const auto read = port.r8;
    port.r8 = [&](u64 address) {
        memory.Write(0x1008, 43, 4);
        return read(address);
    };
    REQUIRE_FALSE(Equip::ReadLink(port, MemoryKeys(), 0x1000));
}

TEST_CASE("Equipment child names require bounded valid guest strings", "[oot][coop][equip]") {
    auto memory = LinkMemory();
    const auto port = memory.Port();
    const auto keys = MemoryKeys();
    memory.Text(0x4000, "Invalid Name");
    REQUIRE_FALSE(Equip::ReadLink(port, keys, 0x1000));
    memory.Text(0x4000, std::string(64, 'a'));
    REQUIRE_FALSE(Equip::ReadLink(port, keys, 0x1000));
    memory.Text(0x4000, "ValidName");
    memory.bytes.erase(0x4004);
    REQUIRE_FALSE(Equip::ReadLink(port, keys, 0x1000));
}

TEST_CASE("Equipment relationship validates bounded wrapped owner rings and races", "[oot][coop][equip]") {
    Memory memory;
    const auto keys = MemoryKeys();
    constexpr u64 base = 0x1000, owner = 0x2000, manager = 0x3000, indices = 0x4000,
                  pairs = 0x5000, child = 0x6000;
    memory.Write(base + 64, manager, 8);
    memory.Write(owner + 80, indices, 8);
    memory.Write(owner + 88, 2, 4);
    memory.Write(owner + 92, 1, 4);
    memory.Write(owner + 96, 2, 4);
    memory.Write(manager + 16, pairs, 8);
    memory.Write(manager + 24, 2, 4);
    memory.Write(indices, 0, 4);
    memory.Write(indices + 4, 1, 4);
    memory.Write(pairs, owner, 8);
    memory.Write(pairs + 16, child, 8);
    memory.Write(pairs + 32, owner, 8);
    memory.Write(pairs + 48, child + 0x100, 8);
    auto port = memory.Port();
    REQUIRE(Equip::Related(port, keys, base, owner, child));
    REQUIRE_FALSE(Equip::Related(port, keys, base, owner, child + 1));
    memory.Write(indices + 4, 2, 4);
    REQUIRE_FALSE(Equip::Related(port, keys, base, owner, child));
    memory.Write(indices + 4, 1, 4);
    memory.Write(owner + 96, 3, 4);
    REQUIRE_FALSE(Equip::Related(port, keys, base, owner, child));
    memory.Write(owner + 96, 2, 4);
    const auto read = port.r64;
    port.r64 = [&](u64 address) {
        if (address == pairs + 16) memory.Write(owner + 96, 1, 4);
        return read(address);
    };
    REQUIRE_FALSE(Equip::Related(port, keys, base, owner, child));
}

TEST_CASE("Equipment controller rejects stale owner returns and never reuses generation scratch", "[oot][coop][equip]") {
    ControllerFixture fixture;
    fixture.UntilCall();
    REQUIRE(fixture.controller->Calling());
    REQUIRE(fixture.calls.size() == 1);
    REQUIRE(fixture.calls[0].entry == ControllerFixture::CreateEntry);
    const auto old_cell = fixture.calls[0].args[2];
    REQUIRE(old_cell == ControllerFixture::Scratch);
    REQUIRE(fixture.memory.Read(old_cell, 8) == old_cell + 8);

    fixture.memory.Write(ControllerFixture::Actor + 0x118, 11, 4);
    fixture.memory.Write(ControllerFixture::Record + 8, 11, 4);
    fixture.root.token.generation = 11;
    fixture.controller->Returned(fixture.port, 0);
    REQUIRE_FALSE(fixture.controller->Calling());
    REQUIRE_FALSE(fixture.controller->Blocked(ControllerFixture::Actor, 11));
    fixture.UntilCall();
    REQUIRE(fixture.calls.size() == 2);
    REQUIRE(fixture.calls[1].entry == ControllerFixture::CreateEntry);
    REQUIRE(fixture.calls[1].args[2] == old_cell + Equip::TreeBytes);
    REQUIRE(fixture.memory.Read(old_cell, 8) == old_cell + 8);
    REQUIRE(fixture.controller->Blocked(ControllerFixture::Actor, 10));
    REQUIRE_FALSE(fixture.controller->Blocked(ControllerFixture::Actor, 11));
    REQUIRE(std::none_of(fixture.logs.begin(), fixture.logs.end(), [](const std::string& line) {
        return line.find("request refused") != std::string::npos;
    }));
    fixture.controller->Returned(fixture.port, 1);
}

TEST_CASE("Equipment controller timeout retires and parks without another create", "[oot][coop][equip]") {
    ControllerFixture fixture;
    fixture.UntilCall();
    fixture.controller->Returned(fixture.port, 1);
    for (unsigned i = 0; i < 1000 && !fixture.controller->Blocked(ControllerFixture::Actor, 10); ++i) {
        if (fixture.Frame()) fixture.controller->Returned(fixture.port, 1);
    }
    REQUIRE(fixture.controller->Blocked(ControllerFixture::Actor, 10));
    REQUIRE_FALSE(fixture.controller->Recreate(ControllerFixture::Actor, 10));
    REQUIRE(std::any_of(fixture.logs.begin(), fixture.logs.end(), [](const std::string& line) {
        return line.find("timeout/quarantined") != std::string::npos;
    }));
    for (unsigned i = 0; i < 60; ++i) {
        if (fixture.Frame()) fixture.controller->Returned(fixture.port, 1);
    }
    REQUIRE(std::count_if(fixture.calls.begin(), fixture.calls.end(), [](const ControllerFixture::Call& call) {
        return call.entry == ControllerFixture::CreateEntry;
    }) == 1);
    REQUIRE(std::any_of(fixture.calls.begin(), fixture.calls.end(), [](const ControllerFixture::Call& call) {
        return call.entry == ControllerFixture::ParkEntry && call.args[0] == ControllerFixture::Actor;
    }));
}

TEST_CASE("Equipment boots guard validates compare branch and the whole byte span", "[oot][coop][equip]") {
    const auto guard = BootGuard();
    REQUIRE(guard.Valid());
    for (unsigned change = 0; change < 10; ++change) {
        auto bad = guard;
        switch (change) {
        case 0: bad.at = 0; break;
        case 1: ++bad.at; break;
        case 2: bad.resume = bad.at; break;
        case 3: bad.resume += 4; break;
        case 4: bad.equipment_register = 31; break;
        case 5: bad.slot_register = 6; break;
        case 6: bad.words[0] |= 1U << 22; break; // shifted immediate
        case 7: bad.words[0] |= 1U << 31; break; // 64-bit compare
        case 8: bad.words[1] &= ~1U; break;     // B.EQ instead of B.NE
        case 9: bad.words[1] += 1U << 5; break; // different continuation
        }
        CAPTURE(change);
        REQUIRE_FALSE(bad.Valid());
    }
    auto bad = guard;
    bad.words.resize(1);
    REQUIRE_FALSE(bad.Valid());
    bad.words.resize(129);
    REQUIRE_FALSE(bad.Valid());

    Memory memory;
    Equip::Keys keys;
    keys.guard = guard;
    constexpr u64 base = 0x100000;
    for (size_t i = 0; i < guard.words.size(); ++i)
        memory.Write(base + guard.at + i * 4, guard.words[i], 4);
    const auto port = memory.Port();
    REQUIRE(Equip::VerifyBootGuard(port, keys, base));
    for (size_t i = 0; i < guard.words.size(); ++i) {
        const u64 at = base + guard.at + i * 4;
        memory.Write(at, guard.words[i] ^ 1U, 4);
        REQUIRE_FALSE(Equip::VerifyBootGuard(port, keys, base));
        memory.Write(at, guard.words[i], 4);
    }
    memory.bytes.erase(base + guard.resume - 1);
    REQUIRE_FALSE(Equip::VerifyBootGuard(port, keys, base));
}

TEST_CASE("Equipment guarded compare updates NZCV and preserves other processor flags", "[oot][coop][equip]") {
    struct Case { u32 left, right, nzcv; };
    constexpr std::array cases{Case{0, 0, 0x60000000}, Case{1, 0, 0x20000000},
        Case{0, 1, 0x80000000}, Case{0x80000000, 1, 0x30000000},
        Case{0x7fffffff, 0xffffffff, 0x90000000}, Case{0xffffffff, 0, 0xa0000000}};
    for (const auto& test : cases) {
        for (u32 old_flags = 0; old_flags < 16; ++old_flags) {
            constexpr u32 preserved = 0x01234567;
            REQUIRE(Equip::CompareFlags(test.left, test.right, (old_flags << 28) | preserved) ==
                    (test.nzcv | preserved));
        }
    }
}

TEST_CASE("Equipment guarded boots follow tunic and precede default head", "[oot][coop][equip]") {
    auto names = Names();
    names.rows.push_back({Slot::Boots, 1, 1, "SourceBoots", "PrivateBoots", {}});
    Equip::Appearance appearance;
    appearance.age = 1;
    appearance.tunic = 1;
    appearance.boots = 1;
    appearance.mask = 0;
    Equip::Reconciler reconcile;
    REQUIRE(reconcile.Desired(Packet(appearance), 1));
    auto observations = Empty();
    auto work = reconcile.Step(observations, names, 1, true, false, true);
    REQUIRE(work.action == Action::Create);
    REQUIRE(work.slot == Slot::Tunic);
    reconcile.Issued(work, observations[Index(Slot::Tunic)]);
    REQUIRE(reconcile.Step(observations, names, 1, true, false, true).action == Action::None);
    Child(observations, Slot::Tunic, "PrivateE");
    work = reconcile.Step(observations, names, 1, true, false, true);
    REQUIRE(work.action == Action::Create);
    REQUIRE(work.slot == Slot::Boots);
    reconcile.Issued(work, observations[Index(Slot::Boots)]);
    REQUIRE(reconcile.Step(observations, names, 1, true, false, true).action == Action::None);
    Child(observations, Slot::Boots, "PrivateBoots");
    work = reconcile.Step(observations, names, 1, true, false, true);
    REQUIRE(work.action == Action::Create);
    REQUIRE(work.slot == Slot::Head);

    Equip::Reconciler disabled;
    appearance.tunic = 3;
    appearance.mask = 15;
    REQUIRE(disabled.Desired(Packet(appearance), 1));
    REQUIRE(disabled.Step(Empty(), names, 1, true, false).action == Action::None);
}

TEST_CASE("Equipment boots bypass is scoped to its pending owner and verified request", "[oot][coop][equip]") {
    ControllerFixture fixture{true};
    const u64 pc = ControllerFixture::Base + fixture.keys->guard.at;
    const u32 slot = fixture.keys->slots[Index(Slot::Boots)].create;
    REQUIRE_FALSE(fixture.controller->Guard(fixture.port, pc, ControllerFixture::Equipment, slot));
    fixture.UntilCall();
    REQUIRE(fixture.calls.back().entry == ControllerFixture::CreateEntry + 4);
    REQUIRE_FALSE(fixture.controller->Guard(fixture.port, pc + 4, ControllerFixture::Equipment, slot));
    REQUIRE_FALSE(fixture.controller->Guard(fixture.port, pc, ControllerFixture::Equipment + 8, slot));
    REQUIRE_FALSE(fixture.controller->Guard(fixture.port, pc, ControllerFixture::Equipment, slot + 1));
    const u64 cell = fixture.calls.back().args[2];
    fixture.memory.Text(cell + 8, "WrongBoots");
    REQUIRE_FALSE(fixture.controller->Guard(fixture.port, pc, ControllerFixture::Equipment, slot));
    fixture.memory.Text(cell + 8, "PrivateBoots");
    REQUIRE(fixture.controller->Guard(fixture.port, pc, ControllerFixture::Equipment, slot) ==
            ControllerFixture::Base + fixture.keys->guard.resume);
    fixture.memory.Write(ControllerFixture::Record + 8, 11, 4);
    REQUIRE_FALSE(fixture.controller->Guard(fixture.port, pc, ControllerFixture::Equipment, slot));
    fixture.controller->Returned(fixture.port, 1);
}
