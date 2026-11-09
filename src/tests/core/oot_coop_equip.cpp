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

struct ControllerFixture {
    static constexpr u64 Base = 0x1000, Actor = 0x10000, Record = 0x11000,
                         Equipment = 0x12000, Holder = 0x13000, Param = 0x14000,
                         NameAt = 0x15000, Model = 0x16000, Table = 0x80000, TableBytes = 0x400;
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

    ControllerFixture() {
        *keys = MemoryKeys();
        keys->enabled = true;
        keys->fields.insert({{"holder", 0x100}, {"equipment", 0x20}, {"owner", 0x28},
                             {"param", 0x30}, {"pouch", 0}, {"sheath_user", 1},
                             {"actor_record", 0x110}, {"actor_id", 0x118},
                             {"model", 0x18}, {"bookkeeping", 0x200}, {"bookkeeping_stride", 8},
                             {"request_bits", 0x700}, {"error_flags", 0x704}, {"error_mask", 1},
                             {"user_cell", 0x800}, {"user_equipment", 0x8},
                             {"user", 0x38}, {"guard", 0}, {"guard_mask", 0x10}});
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
            if (at < Table || at + size > Table + TableBytes) return false;
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
        controller = std::make_unique<Equip::Controller>(keys, std::move(names), ghost, Base, Table, TableBytes);
    }

    /// A validated child at `slot`, related to the owner through a one-entry dependency ring.
    void AddChild(Slot slot, u64 child, u32 generation, std::string_view name) {
        const u64 link = Equipment + keys->slots[Index(slot)].link, record = child + 0x800,
                  text = child + 0x900, manager = 0x30000, indices = 0x31000, pairs = 0x32000;
        memory.Write(link, record, 8);
        memory.Write(link + 8, generation, 4);
        memory.Write(record, child, 8);
        memory.Write(record + 8, generation, 4);
        memory.Write(child + 0x120, text, 8);
        memory.Write(child + 0x130, 6, 4);
        memory.Text(text, name);
        // Each child appends one (owner, child) pair to the owner's ring.
        const u32 n = related++;
        memory.Write(Base + 64, manager, 8);
        memory.Write(Actor + 80, indices, 8);
        memory.Write(Actor + 88, 8, 4);
        memory.Write(Actor + 92, 0, 4);
        memory.Write(Actor + 96, n + 1, 4);
        memory.Write(manager + 16, pairs, 8);
        memory.Write(manager + 24, n + 1, 4);
        memory.Write(indices + 4 * n, n, 4);
        memory.Write(pairs + 32 * n, Actor, 8);
        memory.Write(pairs + 32 * n + 16, child, 8);
    }
    u32 related{};
    bool Frame(bool eligible = true) {
        root.eligible = eligible;
        return controller->Frame(port, {root}, root.token.scene, true);
    }
    void UntilCall() {
        for (unsigned i = 0; i < 32; ++i) if (Frame()) return;
        FAIL("expected a bounded equipment call");
    }
};
} // namespace

TEST_CASE("Equipment word exhausts each mod field", "[core][oot_coop][equip]") {
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

TEST_CASE("Equipment rejects malformed words and contradictory age", "[core][oot_coop][equip]") {
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

TEST_CASE("Equipment packet requires actual JSON integers", "[core][oot_coop][equip]") {
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

TEST_CASE("Equipment sequence tracks known settled changes and wraps", "[core][oot_coop][equip]") {
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

TEST_CASE("Equipment reconciliation waits for sword and private sheath before other slots", "[core][oot_coop][equip]") {
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

TEST_CASE("Equipment replacements coalesce behind their predecessor generation", "[core][oot_coop][equip]") {
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
    REQUIRE(reconcile.Desired(Packet(appearance, 65534), 0)); // arrival order, never eqseq, decides
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

TEST_CASE("Equipment cannot skip an unreadable or busy earlier slot", "[core][oot_coop][equip]") {
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

TEST_CASE("Equipment timeout counts eligible updates and latches quarantine", "[core][oot_coop][equip]") {
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

TEST_CASE("Equipment none defaults to preservation and boots stay blocked", "[core][oot_coop][equip]") {
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

TEST_CASE("Equipment native arguments keep creation and dynamic slot domains separate", "[core][oot_coop][equip]") {
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

TEST_CASE("Equipment child links reject stale records and retiring lifecycle", "[core][oot_coop][equip]") {
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

TEST_CASE("Equipment links revalidate generation after reading the child", "[core][oot_coop][equip]") {
    auto memory = LinkMemory();
    auto port = memory.Port();
    const auto read = port.r8;
    port.r8 = [&](u64 address) {
        memory.Write(0x1008, 43, 4);
        return read(address);
    };
    REQUIRE_FALSE(Equip::ReadLink(port, MemoryKeys(), 0x1000));
}

TEST_CASE("Equipment child names require bounded valid guest strings", "[core][oot_coop][equip]") {
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

TEST_CASE("Equipment relationship validates bounded wrapped owner rings and races", "[core][oot_coop][equip]") {
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

TEST_CASE("Equipment controller rejects stale owner returns and writes each name cell once", "[core][oot_coop][equip]") {
    ControllerFixture fixture;
    fixture.UntilCall();
    REQUIRE(fixture.controller->Calling());
    REQUIRE(fixture.calls.size() == 1);
    REQUIRE(fixture.calls[0].entry == ControllerFixture::CreateEntry);
    const auto cell = fixture.calls[0].args[2];
    REQUIRE(cell == ControllerFixture::Table);
    REQUIRE(fixture.memory.Read(cell, 8) == cell + 8);
    const auto before = fixture.memory.bytes;

    // The owner is re-created (new generation) while its request is in flight.
    fixture.memory.Write(ControllerFixture::Actor + 0x118, 11, 4);
    fixture.memory.Write(ControllerFixture::Record + 8, 11, 4);
    fixture.root.token.generation = 11;
    fixture.controller->Returned(fixture.port, 0); // would quarantine if accepted
    REQUIRE_FALSE(fixture.controller->Calling());
    REQUIRE_FALSE(fixture.controller->Blocked(ControllerFixture::Actor, 10));
    REQUIRE_FALSE(fixture.controller->Blocked(ControllerFixture::Actor, 11));
    fixture.UntilCall();
    REQUIRE(fixture.calls.size() == 2);
    // The same allowlisted name reuses its immutable cell; nothing in the table was rewritten.
    REQUIRE(fixture.calls[1].args[2] == cell);
    for (const auto& [address, value] : before)
        if (address >= ControllerFixture::Table && address < ControllerFixture::Table + ControllerFixture::TableBytes)
            REQUIRE(fixture.memory.bytes.at(address) == value);
    REQUIRE(std::none_of(fixture.logs.begin(), fixture.logs.end(), [](const std::string& line) {
        return line.find("request refused") != std::string::npos;
    }));
    fixture.controller->Returned(fixture.port, 1);
}

TEST_CASE("Equipment name table is bounded, aligned and refuses unsafe names", "[core][oot_coop][equip]") {
    Memory memory;
    auto port = memory.Port();
    std::vector<std::pair<u64, size_t>> writes;
    port.write = [&](u64 at, const void* bytes, size_t size) {
        writes.emplace_back(at, size);
        const auto* data = static_cast<const u8*>(bytes);
        for (size_t i = 0; i < size; ++i) memory.bytes[at + i] = data[i];
        return true;
    };
    port.log = [](std::string_view) {};
    Equip::NameTable table{0x1000, 0x40};
    const auto a = table.Cell(port, "Abc");
    REQUIRE(a == 0x1000);
    REQUIRE(memory.Read(0x1000, 8) == 0x1008);
    REQUIRE(table.Cell(port, "Abc") == a);
    REQUIRE(writes.size() == 1);
    const auto b = table.Cell(port, "Defgh");
    REQUIRE(b == 0x1010);
    REQUIRE(*b % 8 == 0);
    REQUIRE_FALSE(table.Cell(port, "bad name"));
    REQUIRE_FALSE(table.Cell(port, std::string(40, 'x'))); // does not fit: refused, not truncated
    REQUIRE(table.Cell(port, "Ij") == 0x1020);
    REQUIRE_FALSE(table.Park(port)); // 48 bytes no longer fit
}

TEST_CASE("Equipment suspends, never retires, an owner that is only ineligible", "[core][oot_coop][equip]") {
    ControllerFixture fixture;
    fixture.UntilCall();
    fixture.controller->Returned(fixture.port, 1);
    for (unsigned i = 0; i < 2000; ++i) REQUIRE_FALSE(fixture.Frame(false));
    REQUIRE_FALSE(fixture.controller->Blocked(ControllerFixture::Actor, 10));
    REQUIRE(std::none_of(fixture.logs.begin(), fixture.logs.end(), [](const std::string& line) {
        return line.find("retired") != std::string::npos || line.find("quarantined") != std::string::npos;
    }));
    // Back in view: the pending request resumes its timeout from where it stood, once.
    fixture.AddChild(Slot::Sword, 0x40000, 77, "PrivateA");
    fixture.AddChild(Slot::Sheath, 0x50000, 78, "PrivateCaseA");
    // Field C15: the request bits stay set while the slots hold their children; that is settled.
    fixture.memory.Write(ControllerFixture::Equipment + 0x700, 1U | (1U << 5), 4);
    for (unsigned i = 0; i < 32 && !fixture.controller->Calling(); ++i) fixture.Frame();
    REQUIRE(fixture.controller->Calling());
    REQUIRE(fixture.calls.back().entry == ControllerFixture::CreateEntry + 8); // actual-state query
}

TEST_CASE("Equipment controller timeout quarantines, parks its children once and never re-creates", "[core][oot_coop][equip]") {
    ControllerFixture fixture;
    fixture.UntilCall();
    fixture.controller->Returned(fixture.port, 1);
    // A wrong child arrives (not the requested name): the request never settles.
    fixture.AddChild(Slot::Sword, 0x40000, 77, "PrivateB");
    for (unsigned i = 0; i < 1000 && !fixture.controller->Blocked(ControllerFixture::Actor, 10); ++i) {
        if (fixture.Frame()) fixture.controller->Returned(fixture.port, 1);
    }
    REQUIRE(fixture.controller->Blocked(ControllerFixture::Actor, 10));
    REQUIRE_FALSE(fixture.controller->Recreate(ControllerFixture::Actor, 10));
    REQUIRE(std::any_of(fixture.logs.begin(), fixture.logs.end(), [](const std::string& line) {
        return line.find("timeout/quarantined") != std::string::npos;
    }));
    for (unsigned i = 0; i < 700; ++i) {
        if (fixture.Frame()) fixture.controller->Returned(fixture.port, 1);
    }
    REQUIRE(std::count_if(fixture.calls.begin(), fixture.calls.end(), [](const ControllerFixture::Call& call) {
        return call.entry == ControllerFixture::CreateEntry;
    }) == 1);
    REQUIRE(std::count_if(fixture.calls.begin(), fixture.calls.end(), [](const ControllerFixture::Call& call) {
        return call.entry == ControllerFixture::ParkEntry && call.args[0] == 0x40000;
    }) == 1);
    // Still listed by the ghost driver: the quarantine holds past the park window.
    REQUIRE(fixture.controller->Blocked(ControllerFixture::Actor, 10));
}

TEST_CASE("Equipment forgets a retired owner once the game deleted it", "[core][oot_coop][equip]") {
    ControllerFixture fixture;
    fixture.UntilCall();
    fixture.controller->Returned(fixture.port, 1);
    fixture.controller->Invalidate(); // scene change
    fixture.memory.Write(ControllerFixture::Record + 8, 12, 4); // the record now names another actor
    fixture.Frame();
    REQUIRE_FALSE(fixture.controller->Blocked(ControllerFixture::Actor, 10));
    // Many owners over a long session never exhaust a budget: each new generation gets a tree.
    for (u32 generation = 20; generation < 60; ++generation) {
        fixture.memory.Write(ControllerFixture::Actor + 0x118, generation, 4);
        fixture.memory.Write(ControllerFixture::Record + 8, generation, 4);
        fixture.root.token.generation = generation;
        const auto calls = fixture.calls.size();
        for (unsigned i = 0; i < 32 && fixture.calls.size() == calls; ++i)
            if (fixture.Frame()) fixture.controller->Returned(fixture.port, 1);
        REQUIRE(fixture.calls.size() > calls);
    }
    REQUIRE(std::none_of(fixture.logs.begin(), fixture.logs.end(), [](const std::string& line) {
        return line.find("budget") != std::string::npos;
    }));
}

TEST_CASE("Equipment sender finds the resident player by reads and reports settled base appearance", "[core][oot_coop][equip]") {
    ControllerFixture fixture;
    auto& memory = fixture.memory;
    constexpr u64 Q = 0x20000, Player = 0x21000, Holder = 0x22000, E = 0x23000, Record = 0x24000, Name = 0x25000;
    memory.Write(ControllerFixture::Base + 0x800, Q, 8);
    memory.Write(Q + 8, E, 8);
    memory.Write(E + 0x28, Player, 8);
    memory.Write(Player + 0x100, Holder, 8);
    memory.Write(Holder + 0x20, E, 8);
    memory.Write(Player + 0x110, Record, 8);
    memory.Write(Player + 0x118, 0x77, 4);
    memory.Write(Player + 0x120, Name, 8);
    memory.Write(Player + 0x130, 4, 4);
    memory.Write(Record, Player, 8);
    memory.Write(Record + 8, 0x77, 4);
    memory.Text(Name, "SyntheticYoung");
    for (const auto& slot : fixture.keys->slots) {
        memory.Write(E + slot.link, 0, 8);
        memory.Write(E + slot.link + 8, 0xffffffff, 4);
        memory.Write(E + slot.handle, 0, 8);
        memory.Write(E + 0x200 + slot.create * 8, 0, 8);
    }
    memory.Write(E + 0x700, 0, 4);
    memory.Write(E + 0x704, 0, 4);
    fixture.root.desired.reset(); // no ghost work: only the sender runs
    const auto frame = [&](bool send) {
        if (fixture.controller->Frame(fixture.port, {fixture.root}, fixture.root.token.scene, true, send, true))
            fixture.controller->Returned(fixture.port, 2);
    };
    for (unsigned i = 0; i < 200; ++i) frame(false);
    REQUIRE_FALSE(fixture.controller->Local(true)); // never sampled before negotiation
    REQUIRE(fixture.calls.empty());
    for (unsigned i = 0; i < 200; ++i) frame(true);
    REQUIRE(fixture.calls.empty()); // no weapon children: no native state query, pure reads only
    const auto packet = fixture.controller->Local(true);
    REQUIRE(packet);
    REQUIRE(packet->word == 0x10000000U); // none/none, base clothes, default head, child
    REQUIRE_FALSE(fixture.controller->Local(false)); // contradicts the outer age
    REQUIRE(std::count_if(fixture.logs.begin(), fixture.logs.end(), [](const std::string& line) {
        return line.find("local player") != std::string::npos;
    }) == 1);
    memory.Text(Name, "SomeoneElse");
    for (unsigned i = 0; i < 100; ++i) frame(true);
    REQUIRE_FALSE(fixture.controller->Local(true));
}

TEST_CASE("Equipment treats a request bit without a child as in flight", "[core][oot_coop][equip]") {
    ControllerFixture fixture;
    fixture.memory.Write(ControllerFixture::Equipment + 0x700, 1, 4); // sword bit, no sword child
    for (unsigned i = 0; i < 64; ++i) REQUIRE_FALSE(fixture.Frame());
    REQUIRE(fixture.calls.empty());
    fixture.memory.Write(ControllerFixture::Equipment + 0x700, 0, 4);
    fixture.UntilCall();
    REQUIRE(fixture.calls.back().entry == ControllerFixture::CreateEntry);
}

TEST_CASE("Equipment defers replacing an occupied static slot", "[core][oot_coop][equip]") {
    Equip::Reconciler reconcile;
    Equip::Appearance appearance{0, 0, 0, 0, 3, 3, 1, 0, 0}; // child, no weapons, mask 1
    REQUIRE(reconcile.Desired(Packet(appearance), 0));
    auto observations = Empty();
    const auto names = Names();
    // Empty head slot: the mask is created.
    auto work = reconcile.Step(observations, names, 0, true, false);
    REQUIRE(work.action == Action::Create);
    REQUIRE(work.slot == Slot::Head);
    // Occupied by the default head: no replacement request, the slot is reported deferred.
    observations = Empty();
    Child(observations, Slot::Head, "PrivateF");
    reconcile = {};
    REQUIRE(reconcile.Desired(Packet(appearance), 0));
    REQUIRE(reconcile.Step(observations, names, 0, true, false).action == Action::None);
    REQUIRE(reconcile.Deferred(Slot::Head));
    for (unsigned i = 0; i < 400; ++i) REQUIRE(reconcile.Step(observations, names, 0, true, false).action == Action::None);
    REQUIRE_FALSE(reconcile.Quarantined());
    appearance.mask = 0; // back to the default head the slot already holds
    REQUIRE(reconcile.Desired(Packet(appearance, 2), 0));
    REQUIRE(reconcile.Step(observations, names, 0, true, false).action == Action::None);
    REQUIRE_FALSE(reconcile.Deferred(Slot::Head));
}
