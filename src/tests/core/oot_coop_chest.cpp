// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <limits>
#include <map>
#include <catch2/catch_test_macros.hpp>
#include "core/arm/dynarmic/oot_coop_chest.h"
#include "core/arm/dynarmic/oot_coop_sync.h"

namespace {
namespace Coop = Core::OotCoop;
namespace Chest = Coop::Chest;
namespace World = Coop::World;
constexpr u32 Key = 101;
constexpr Chest::EventToken Event{0x7000, 1};
Chest::Identity Identity(u32 key = Key, u64 actor = 0x4000, u64 lifetime = 7) {
    return {{actor, actor + 0x100, lifetime, 1}, 123456789, 202, key};
}
Coop::Change Give(std::string origin = "peer", u64 transaction = 51, u32 part = 0,
                  bool final = true, u32 key = Key) {
    Coop::Change change{Coop::Kind::Item, std::move(origin), 1,
                        Coop::PackItem({2, 0, {0, 0, 0, 0}}), 1};
    change.name = "Test_ChestReward";
    change.source = World::Origin::Chest(key);
    change.chest_transaction = transaction;
    change.chest_part = part;
    change.chest_final = final;
    return change;
}
struct Memory {
    std::map<u64, u8> bytes;
    mutable size_t byte_reads{};
    void Put(u64 address, u64 value, size_t count) {
        for (size_t i = 0; i < count; ++i) bytes[address + i] = static_cast<u8>(value >> (i * 8));
    }
    bool IsValidVirtualAddressRange(u64 address, size_t count) const {
        for (size_t i = 0; i < count; ++i) if (!bytes.contains(address + i)) return false;
        return true;
    }
    u8 Read8(u64 address) const { ++byte_reads; return bytes.at(address); }
    u32 Read32(u64 address) const { return static_cast<u32>(Read(address, 4)); }
    u64 Read64(u64 address) const { return Read(address, 8); }
    u64 Read(u64 address, size_t count) const {
        u64 result{};
        for (size_t i = 0; i < count; ++i) result |= u64{bytes.at(address + i)} << (i * 8);
        return result;
    }
};
struct Fixture {
    static constexpr u64 Base = 0x100000, Node = 0x1000, Actor = 0x4000;
    static constexpr u64 Instance = 18446744073709551601ULL;
    Memory memory;
    Chest::Config config;
    std::map<u32, u32> members;
    Fixture() {
        config.selector_vtable = 0x500;
        config.input_offset = 0x40;
        config.input_value = 9;
        config.input_pointer_mask = 3;
        config.reserved_offset = 0x60;
        config.current_offset = 0x61;
        config.flags_offset = 0x62;
        config.invalid_child = 0xFE;
        config.transition_mask = 0x40;
        config.owner_chain = {0x10, 0x20, 0x30};
        config.metadata_offset = 0x80;
        config.metadata_key_offset = 0x90;
        config.link = {0x100, 0x110, 0x120, 0x130};
        memory.Put(Node, Base + config.selector_vtable, 8);
        memory.Put(Node + 0x10, 0x2000, 8);
        memory.Put(0x2020, 0x3000, 8);
        memory.Put(0x3030, Actor, 8);
        memory.Put(Node + 0x40, 0x5003, 8);
        memory.Put(0x5000, 9, 4);
        memory.Put(Node + 0x60, 4, 1);
        memory.Put(Node + 0x61, 5, 1);
        memory.Put(Node + 0x62, 0, 1);
        memory.Put(Actor + 0x80, 0x6000, 8);
        memory.Put(0x6090, Instance, 8);
        memory.Put(Actor + 0x100, 0x8000, 8);
        memory.Put(Actor + 0x110, 7, 4);
        memory.Put(0x8120, Actor, 8);
        memory.Put(0x8130, 7, 4);
        members.emplace(Common::Nx2EventTrace::Murmur3("18446744073709551601"), Key);
    }
    std::optional<Chest::Identity> Read() {
        return Chest::ReadInteraction(memory, Node, Base, 1, config, members);
    }
};
} // namespace

TEST_CASE("OoT chest interaction resolves decimal instance identity and checked lifetime", "[oot_coop]") {
    Fixture f;
    const auto identity = f.Read();
    REQUIRE(identity);
    CHECK(identity->key == Key);
    CHECK(identity->instance == Fixture::Instance);
    CHECK(identity->token.actor == Fixture::Actor);
    CHECK(identity->token.record == 0x8000);
    CHECK(identity->token.lifetime == 7);
    CHECK(identity->token.epoch == 1);
    CHECK(identity->member == Common::Nx2EventTrace::Murmur3("18446744073709551601"));
    CHECK(Chest::ReadNodeActor(f.memory, Fixture::Node, f.config) == Fixture::Actor);
}

TEST_CASE("OoT chest interaction rejects every nontransition and invalid identity", "[oot_coop]") {
    Fixture f;
    SECTION("wrong selector") { f.memory.Put(Fixture::Node, 0x1234, 8); }
    SECTION("wrong input") { f.memory.Put(0x5000, 8, 4); }
    SECTION("missing input") { f.memory.bytes.erase(0x5000); }
    SECTION("invalid reserved child") { f.memory.Put(Fixture::Node + 0x60, 0xFE, 1); }
    SECTION("current child") { f.memory.Put(Fixture::Node + 0x60, 5, 1); }
    SECTION("transition inhibited") { f.memory.Put(Fixture::Node + 0x62, 0x40, 1); }
    SECTION("unreadable guard") { f.memory.bytes.erase(Fixture::Node + 0x62); }
    SECTION("null actor") { f.memory.Put(0x3030, 0, 8); }
    SECTION("unreadable owner") { f.memory.bytes.erase(0x2020); }
    SECTION("null metadata") { f.memory.Put(Fixture::Actor + 0x80, 0, 8); }
    SECTION("unmapped member") {
        f.members.clear();
        CHECK(Chest::SelectedInteraction(f.memory, Fixture::Node, Fixture::Base, f.config));
    }
    SECTION("wrong actor lifetime record") { f.memory.Put(0x8120, 0x9000, 8); }
    SECTION("generation changed") { f.memory.Put(0x8130, 8, 4); }
    SECTION("unreadable generation") { f.memory.bytes.erase(0x8130); }
    SECTION("overflow base") { f.config.selector_vtable = std::numeric_limits<u64>::max(); }
    CHECK_FALSE(f.Read());
}

TEST_CASE("OoT chest checked reads reject overflow before memory access", "[oot_coop]") {
    Fixture f;
    CHECK_FALSE(Chest::ReadActor(f.memory, std::numeric_limits<u64>::max() - 1, 1, f.config, f.members));
    f.config.owner_chain[0] = std::numeric_limits<u64>::max();
    CHECK_FALSE(Chest::ReadNodeActor(f.memory, Fixture::Node, f.config));
}

TEST_CASE("OoT chest begin bind gate return complete groups outer benefits", "[oot_coop]") {
    Chest::Claims claims;
    const auto actor = Identity();
    const u64 attempt = claims.LocalBegin(actor, 1, 10);
    REQUIRE(attempt != 0);
    CHECK(claims.LocalBegin(actor, 1, 11) == attempt);
    CHECK(claims.FindActor(actor.token) == attempt);
    CHECK_FALSE(claims.FindEvent({}));
    const u64 order = claims.Find(Key)->claim_order;
    const auto prelude = claims.Gate(attempt);
    REQUIRE(prelude.decision == Chest::Decision::Allow);
    CHECK(prelude.part == 0);
    CHECK(claims.Returned(attempt, prelude.part, true));
    REQUIRE(claims.BindEvent(attempt, Event));
    CHECK(claims.Attribute(std::nullopt, Event) == attempt);
    const auto content = claims.Gate(attempt);
    REQUIRE(content.decision == Chest::Decision::Allow);
    CHECK(content.transaction == prelude.transaction);
    CHECK(content.part == 1);
    CHECK(claims.Returned(attempt, content.part, true));
    claims.Complete(Key);
    CHECK(claims.Find(Key)->final_part == 1);
    CHECK(claims.Find(Key)->claim_order == order);
    CHECK(claims.Gate(attempt).decision == Chest::Decision::Skip);
    claims.EndEvent(Event);
    CHECK_FALSE(claims.FindActor(actor.token));
    CHECK_FALSE(claims.FindEvent(Event));
    CHECK(claims.Find(Key)->applied.size() == 2);
    CHECK(claims.FindAttempt(attempt)->ended);
}

TEST_CASE("OoT chest receiver follows first source decisions", "[oot_coop]") {
    Chest::Claims claims;
    auto peer = Give();
    SECTION("peer give first then local opens before replay") {
        REQUIRE(claims.AcceptRemote(peer));
        const auto attempt = claims.LocalBegin(Identity(), 1, 10);
        REQUIRE(claims.BindEvent(attempt, Event));
        CHECK(claims.Gate(attempt).decision == Chest::Decision::Skip);
        claims.EndEvent(Event);
        REQUIRE(claims.StartRemote(peer));
        claims.RemoteReturned(peer, true);
        CHECK(claims.Find(Key)->outcome == Chest::Outcome::RemoteApplied);
    }
    SECTION("peer claim first without a benefit remains pending") {
        claims.RemoteClaim(peer.source, peer.origin);
        const auto attempt = claims.LocalBegin(Identity(), 1, 10);
        CHECK(claims.Gate(attempt).decision == Chest::Decision::Skip);
        CHECK(claims.Find(Key)->outcome == Chest::Outcome::Pending);
        CHECK(claims.Find(Key)->applied.empty());
    }
    SECTION("peer paid then loaded actor opens") {
        REQUIRE(claims.AcceptRemote(peer));
        REQUIRE(claims.StartRemote(peer));
        claims.RemoteReturned(peer, true);
        const auto attempt = claims.LocalBegin(Identity(), 1, 10);
        CHECK(claims.Gate(attempt).decision == Chest::Decision::Skip);
        CHECK_FALSE(claims.AcceptRemote(Give("peer", 100)));
    }
    SECTION("local interaction wins before incoming flag and give") {
        const auto attempt = claims.LocalBegin(Identity(), 1, 10);
        claims.RemoteClaim(peer.source, peer.origin);
        CHECK(claims.Owner(peer.source) == "us");
        CHECK_FALSE(claims.AcceptRemote(peer));
        CHECK(claims.Gate(attempt).decision == Chest::Decision::Allow);
    }
    SECTION("local payment survives event end and late peer") {
        const auto attempt = claims.LocalBegin(Identity(), 1, 10);
        REQUIRE(claims.BindEvent(attempt, Event));
        const auto gate = claims.Gate(attempt);
        REQUIRE(claims.Returned(attempt, gate.part, true));
        CHECK_FALSE(claims.AcceptRemote(peer));
        claims.EndEvent(Event);
        CHECK_FALSE(claims.AcceptRemote(peer));
        CHECK_FALSE(claims.CanApply(peer));
    }
    SECTION("competing peer origins never mix parts") {
        REQUIRE(claims.AcceptRemote(peer));
        CHECK_FALSE(claims.AcceptRemote(Give("other", 51, 1)));
        CHECK_FALSE(claims.AcceptRemote(Give("peer", 52)));
        CHECK(claims.Owner(peer.source) == "peer");
    }
    SECTION("unrelated untagged give retains ordinary behavior") {
        REQUIRE(claims.LocalBegin(Identity(), 1, 10));
        peer.source = {};
        CHECK(claims.AcceptRemote(peer));
        CHECK(claims.CanApply(peer));
        CHECK_FALSE(claims.Attribute(std::nullopt, {0x9000, 1}));
    }
}

TEST_CASE("OoT simultaneous chest starts retain one local benefit per client", "[oot_coop]") {
    Chest::Claims a, b;
    const auto aa = a.LocalBegin(Identity(), 1, 1);
    const auto bb = b.LocalBegin(Identity(), 1, 1);
    CHECK_FALSE(a.AcceptRemote(Give("b")));
    CHECK_FALSE(b.AcceptRemote(Give("a")));
    CHECK(a.Gate(aa).decision == Chest::Decision::Allow);
    CHECK(b.Gate(bb).decision == Chest::Decision::Allow);
}

TEST_CASE("OoT chest receipts suppress retransmit and second event ids", "[oot_coop]") {
    Chest::Claims claims;
    auto part = Give("peer", 51, 0, false);
    REQUIRE(claims.AcceptRemote(part));
    REQUIRE(claims.StartRemote(part));
    CHECK_FALSE(claims.CanApply(part));
    claims.RemoteReturned(part, true);
    part.seq = 99;
    CHECK_FALSE(claims.AcceptRemote(part));
    CHECK_FALSE(claims.AcceptRemote(Give("peer", 52, 0, false)));
    auto last = Give("peer", 51, 1, true);
    REQUIRE(claims.AcceptRemote(last));
    REQUIRE(claims.StartRemote(last));
    claims.RemoteReturned(last, true);
    CHECK(claims.Find(Key)->outcome == Chest::Outcome::RemoteApplied);
    CHECK(claims.Find(Key)->applied.size() == 2);
    last.seq = 100;
    CHECK_FALSE(claims.AcceptRemote(last));
}

TEST_CASE("OoT chest out of order parts wait for the complete transaction", "[oot_coop]") {
    Chest::Claims claims;
    auto last = Give("peer", 51, 1, true);
    REQUIRE(claims.AcceptRemote(last));
    REQUIRE(claims.StartRemote(last));
    claims.RemoteReturned(last, true);
    CHECK_FALSE(claims.Find(Key)->closed);
    auto first = Give("peer", 51, 0, false);
    REQUIRE(claims.AcceptRemote(first));
    REQUIRE(claims.StartRemote(first));
    claims.RemoteReturned(first, true);
    CHECK(claims.Find(Key)->closed);
}

TEST_CASE("OoT legacy chest receipts survive new wire sequences", "[oot_coop]") {
    Chest::Claims claims;
    auto legacy = Give("peer", 0, 0, false);
    REQUIRE(claims.AcceptRemote(legacy));
    REQUIRE(claims.StartRemote(legacy));
    claims.RemoteReturned(legacy, true);
    legacy.seq += 10;
    CHECK_FALSE(claims.AcceptRemote(legacy));
}

TEST_CASE("OoT chest baseline is historical and does not invent an owner", "[oot_coop]") {
    Chest::Claims claims;
    claims.RemoteClaim(World::Origin::Chest(Key), "", true);
    CHECK(claims.Owner(World::Origin::Chest(Key)) == "historical");
    CHECK(claims.Find(Key)->applied.empty());
    CHECK_FALSE(claims.AcceptRemote(Give()));
    const auto attempt = claims.LocalBegin(Identity(), 1, 10);
    CHECK(claims.Gate(attempt).decision == Chest::Decision::Skip);
    claims.Complete(Key);
    Chest::Claims empty;
    empty.RemoteClaim(World::Origin::Chest(Key), "");
    CHECK(empty.Owner(World::Origin::Chest(Key)) == "none");
}

TEST_CASE("OoT chest event end releases an unpaid open and keeps an applied prelude", "[oot_coop]") {
    Chest::Claims claims;
    const auto attempt = claims.LocalBegin(Identity(), 1, 10);
    REQUIRE(claims.BindEvent(attempt, Event));
    SECTION("canceled before any benefit or completion: released, a later opener is paid") {
        claims.EndEvent(Event);
        CHECK_FALSE(claims.Find(Key)->selected);
        CHECK_FALSE(claims.FindAttempt(attempt));
        CHECK_FALSE(claims.LocalActive(Key));
        CHECK(claims.AcceptRemote(Give()));
        CHECK(claims.Owner(World::Origin::Chest(Key)) == "peer");
    }
    SECTION("the open completed (setter) without a give: not released, peer copy dropped") {
        claims.Complete(Key);
        claims.EndEvent(Event);
        CHECK(claims.Find(Key)->closed);
        CHECK(claims.Find(Key)->outcome == Chest::Outcome::Unknown);
        CHECK_FALSE(claims.AcceptRemote(Give()));
    }
    SECTION("after prelude") {
        const auto gate = claims.Gate(attempt);
        REQUIRE(claims.Returned(attempt, gate.part, true));
        claims.EndEvent(Event);
        CHECK(claims.Find(Key)->applied.contains(0));
        CHECK(claims.Find(Key)->final_part == 0);
        CHECK_FALSE(claims.AcceptRemote(Give()));
    }
    CHECK_FALSE(claims.FindEvent(Event));
    CHECK_FALSE(claims.FindActor(Identity().token));
}

TEST_CASE("OoT chest confirmed open never times out; an unconfirmed one is released", "[oot_coop]") {
    Chest::Claims claims;
    SECTION("confirmed: the player may leave the item message up indefinitely") {
        const auto attempt = claims.LocalBegin(Identity(), 1, 10);
        REQUIRE(claims.BindEvent(attempt, Event));
        CHECK(claims.Expire(100000).empty());
        CHECK(claims.FindEvent(Event) == attempt);
        const auto gate = claims.Gate(attempt);
        CHECK(gate.decision == Chest::Decision::Allow);
        CHECK(gate.transaction == attempt);
        CHECK_FALSE(claims.AcceptRemote(Give()));
    }
    SECTION("unconfirmed and unpaid: ended and released, the peer queue is not wedged") {
        const auto other = claims.LocalBegin(Identity(102, 0x5000), 1, 10);
        claims.RemoteClaim(World::Origin::Chest(102), "peer"); // a hint only: local was first
        const auto attempt = claims.LocalBegin(Identity(), 1, 10);
        CHECK(claims.Expire(1809).empty());
        const auto expired = claims.Expire(1810);
        CHECK(expired == std::vector<u64>{other, attempt});
        CHECK_FALSE(claims.FindAttempt(attempt));
        CHECK_FALSE(claims.LocalActive(Key));
        CHECK_FALSE(claims.Find(Key)->selected);
        CHECK(claims.AcceptRemote(Give()));
        CHECK(claims.Expire(4000).empty());
    }
    SECTION("unconfirmed with a give in flight is kept until its return") {
        const auto attempt = claims.LocalBegin(Identity(), 1, 10);
        const auto gate = claims.Gate(attempt);
        REQUIRE(gate.decision == Chest::Decision::Allow);
        CHECK(claims.Expire(4000).empty());
        REQUIRE(claims.Returned(attempt, gate.part, true));
        CHECK(claims.Expire(4000) == std::vector<u64>{attempt});
        CHECK(claims.Find(Key)->closed);
        CHECK(claims.Find(Key)->final_part == 0);
        CHECK_FALSE(claims.AcceptRemote(Give()));
    }
}

TEST_CASE("OoT chest released record is re-selected by the next local opener", "[oot_coop]") {
    Chest::Claims claims;
    const auto first = claims.LocalBegin(Identity(), 1, 1);
    REQUIRE(claims.BindEvent(first, Event));
    const auto second = claims.LocalBegin(Identity(Key, 0x5000, 9), 1, 2); // a reloaded actor
    REQUIRE(second != first);
    CHECK(claims.Gate(second).decision == Chest::Decision::Skip); // the first still owns it
    claims.EndEvent(Event);                                        // canceled, unpaid: released
    const auto gate = claims.Gate(second);
    CHECK(gate.decision == Chest::Decision::Allow);
    CHECK(gate.transaction == second);
    CHECK(claims.Owner(World::Origin::Chest(Key)) == "us");
}

TEST_CASE("OoT chest attribution rejects ambiguity and stale lifetimes", "[oot_coop]") {
    Chest::Claims claims;
    const auto a = claims.LocalBegin(Identity(), 1, 1);
    const auto other = Identity(102, 0x5000);
    const auto b = claims.LocalBegin(other, 1, 1);
    REQUIRE(claims.BindEvent(a, Event));
    CHECK_FALSE(claims.Attribute(other.token, Event));
    CHECK_FALSE(claims.FindActor(Identity(Key, 0x4000, 8).token));
    CHECK_FALSE(claims.FindEvent({Event.pointer, 2}));
    CHECK_FALSE(claims.BindEvent(b, Event));
    CHECK_FALSE(claims.Attribute(Identity().token, Event));
    claims.EndEvent(Event);
    CHECK_FALSE(claims.FindAttempt(a)); // both unpaid: released together, neither selected
    CHECK_FALSE(claims.FindAttempt(b));
    CHECK_FALSE(claims.Find(Key)->selected);
    const auto c = claims.LocalBegin(Identity(103, 0x6000), 1, 3);
    CHECK(claims.BindEvent(c, {Event.pointer, 2}));
    CHECK(claims.FindEvent({Event.pointer, 2}) == c);
}

TEST_CASE("OoT chest failed native return stays unresolved", "[oot_coop]") {
    Chest::Claims claims;
    SECTION("local") {
        const auto attempt = claims.LocalBegin(Identity(), 1, 1);
        const auto gate = claims.Gate(attempt);
        CHECK_FALSE(claims.Returned(attempt, gate.part, false));
        CHECK(claims.Gate(attempt).decision == Chest::Decision::Unknown);
    }
    SECTION("peer") {
        const auto give = Give();
        REQUIRE(claims.AcceptRemote(give));
        REQUIRE(claims.StartRemote(give));
        claims.RemoteReturned(give, false);
        CHECK_FALSE(claims.CanApply(give));
    }
    CHECK(claims.Find(Key)->outcome == Chest::Outcome::Unknown);
    CHECK(claims.Find(Key)->applied.empty());
}

TEST_CASE("OoT chest event activity preserves nested bindings and unreadable samples", "[oot_coop]") {
    Chest::Events events;
    Chest::Claims claims;
    const auto attempt = claims.LocalBegin(Identity(), 1, 1);
    const auto outer = events.Sample(0x7000);
    REQUIRE(outer);
    REQUIRE(claims.BindEvent(attempt, outer));
    const auto nested = events.Sample(0x8000);
    REQUIRE(nested);
    CHECK(nested.generation == outer.generation);
    CHECK(events.TakeEnded().empty());
    CHECK_FALSE(events.Sample(std::nullopt));
    CHECK(events.TakeEnded().empty());
    CHECK(events.Sample(0x7000) == outer);
    CHECK(claims.FindEvent(outer) == attempt);
    CHECK_FALSE(events.Sample(0));
    const auto ended = events.TakeEnded();
    REQUIRE(ended.size() == 2);
    for (const auto event : ended) claims.EndEvent(event);
    CHECK_FALSE(claims.FindEvent(outer));
    const auto reused = events.Sample(0x7000);
    CHECK(reused.pointer == outer.pointer);
    CHECK(reused.generation > outer.generation);
    CHECK(events.TakeEnded().empty());
}

TEST_CASE("OoT chest event contexts are bounded without evicting active bindings", "[oot_coop]") {
    Chest::Events events;
    const auto first = events.Sample(0x1000);
    for (size_t i = 1; i < Chest::Events::Limit; ++i) REQUIRE(events.Sample(0x1000 + i));
    CHECK_FALSE(events.Sample(0x2000));
    CHECK(events.Overflowed());
    CHECK(events.Sample(0x1000) == first);
    CHECK(events.TakeEnded().empty());
}

TEST_CASE("OoT peer chest claim without benefit remains retryable after bounded notice", "[oot_coop]") {
    Chest::Claims claims;
    claims.RemoteClaim(World::Origin::Chest(Key), "peer");
    CHECK(claims.MissingBenefits(10).empty());
    CHECK(claims.MissingBenefits(1809).empty());
    CHECK(claims.MissingBenefits(1810) == std::vector<u32>{Key});
    CHECK(claims.MissingBenefits(4000).empty());
    CHECK(claims.Find(Key)->outcome == Chest::Outcome::Pending);
    CHECK(claims.Find(Key)->applied.empty());
    CHECK(claims.AcceptRemote(Give()));
}

namespace {
nlohmann::json ChestConfigJson() {
    return {{"interaction_pc", "0x2000"}, {"confirm_pc", "0x3000"}, {"return_pc", "0x4000"},
            {"interaction_word", 0x39418001U}, {"confirm_word", 0x94000004U},
            {"return_word", 0xB0000028U}, {"struct_hash", 303}, {"selector_vtable", "0x5000"},
            {"input_offset", 0x40}, {"input_value", 9}, {"input_pointer_mask", 3},
            {"reserved_offset", 0x60}, {"current_offset", 0x61}, {"flags_offset", 0x62},
            {"invalid_child", 0xFE}, {"transition_mask", 0x40},
            {"owner_chain", {0x10, 0x20, 0x30}}, {"metadata_offset", 0x80},
            {"metadata_key_offset", 0x90}, {"link", {0x100, 0x110, 0x120, 0x130}}};
}
struct Registers {
    std::array<u64, 31> regs{};
    std::array<std::array<u64, 2>, 32> vectors{};
    u32 flags{0xA0000000};
    u64 pc{};
    u64 GetRegister(size_t i) const { return regs.at(i); }
    void SetRegister(size_t i, u64 value) { regs.at(i) = value; }
    void SetPC(u64 value) { pc = value; }
};
} // namespace

TEST_CASE("OoT chest private profile config is strict and safe by default", "[oot_coop]") {
    auto json = ChestConfigJson();
    Chest::Config config;
    REQUIRE(Chest::DecodeConfig(json, config));
    CHECK_FALSE(config.enforce);
    CHECK(config.owner_chain == std::array<u64, 3>{0x10, 0x20, 0x30});
    CHECK(config.link == std::array<u64, 4>{0x100, 0x110, 0x120, 0x130});
    SECTION("explicit enforcement") {
        json["enforce"] = true;
        REQUIRE(Chest::DecodeConfig(json, config));
        CHECK(config.enforce);
        return;
    }
    SECTION("unknown field") { json["guessed"] = 1; }
    SECTION("missing layout field") { json.erase("current_offset"); }
    SECTION("missing zero-capable input") { json.erase("input_value"); }
    SECTION("wrong boolean type") { json["enforce"] = 1; }
    SECTION("too wide guard") { json["transition_mask"] = 256; }
    SECTION("wrong chain length") { json["owner_chain"] = {1, 2}; }
    SECTION("overflow code") { json["interaction_pc"] = 0x10000001; }
    SECTION("overflow selector") { json["selector_vtable"] = 0x10000001; }
    SECTION("overflow offset") { json["metadata_key_offset"] = 0x10000; }
    SECTION("overflow link") { json["link"][0] = 0x10000; }
    CHECK_FALSE(Chest::DecodeConfig(json, config));
}

TEST_CASE("OoT chest displaced instructions preserve unrelated registers and flags", "[oot_coop]") {
    namespace Trace = Core::OotGameDataTrace;
    Memory memory;
    Registers jit;
    for (size_t i = 0; i < jit.regs.size(); ++i) jit.regs[i] = 0x1000 + i;
    const auto before = jit.regs;
    const auto vectors = jit.vectors;
    constexpr u64 pc = 0x120020;
    jit.pc = pc + 4;
    Trace::Profile profile;
    Trace::Point point{};
    u32 word{};
    SECTION("interaction LDRB zero extends W1 and reads once") {
        point = Trace::Point::ChestInteraction;
        word = 0x39418001U; // Synthetic LDRB W1,[X0,#0x60].
        memory.Put(before[0] + 0x60, 0xB7, 1);
    }
    SECTION("confirmation BL positive displacement") {
        point = Trace::Point::ChestConfirm;
        word = 0x94000004U; // Synthetic BL PC+16.
    }
    SECTION("confirmation BL negative displacement") {
        point = Trace::Point::ChestConfirm;
        word = 0x97FFFFFCU; // Synthetic BL PC-16.
    }
    SECTION("return ADRP positive displacement") {
        point = Trace::Point::ChestReturn;
        word = 0xB0000028U; // Synthetic ADRP X8,PC-page+5 pages.
    }
    SECTION("return ADRP negative displacement") {
        point = Trace::Point::ChestReturn;
        word = 0xF0FFFFE8U; // Synthetic ADRP X8,PC-page-1 page.
    }
    memory.Put(pc, word, 4);
    profile.sites.push_back({point, 0x20, word});
    CHECK(Trace::Identify(profile, 0x120000, pc, word) == point);
    CHECK(Trace::Identify(profile, 0x120000, pc, word ^ 4) == Trace::Point::None);
    CHECK(Trace::Identify(profile, 0x120000, pc + 4, word) == Trace::Point::None);
    CHECK(Trace::InstructionFor(profile, 0x120000, pc, word) == Trace::Breakpoint);
    CHECK(Trace::CoopPoint(point));
    Trace::Resume(point, jit, memory, pc);
    auto expected = before;
    if (point == Trace::Point::ChestInteraction) {
        expected[1] = 0xB7;
        CHECK(memory.byte_reads == 1);
    }
    if (point == Trace::Point::ChestConfirm) expected[30] = pc + 4;
    if (point == Trace::Point::ChestReturn)
        expected[8] = word == 0xB0000028U ? 0x125000 : 0x11F000;
    CHECK(jit.regs == expected);
    CHECK(jit.flags == 0xA0000000);
    CHECK(jit.vectors == vectors);
    if (point == Trace::Point::ChestConfirm)
        CHECK(jit.pc == (word == 0x94000004U ? pc + 16 : pc - 16));
    else CHECK(jit.pc == pc + 4);
}

TEST_CASE("OoT peer chest replay waits for interaction attempt before event binding", "[oot_coop]") {
    Chest::Claims claims;
    const auto peer = Give();
    REQUIRE(claims.AcceptRemote(peer));
    CHECK_FALSE(claims.LocalActive(Key));
    const auto attempt = claims.LocalBegin(Identity(), 1, 1);
    REQUIRE(attempt);
    CHECK(claims.LocalActive(Key));
    CHECK_FALSE(claims.LocalActive(102));
    CHECK_FALSE(claims.FindEvent({}));
    CHECK(claims.Gate(attempt).decision == Chest::Decision::Skip);
    REQUIRE(claims.BindEvent(attempt, Event));
    CHECK(claims.LocalActive(Key));
    claims.EndEvent(Event);
    CHECK_FALSE(claims.LocalActive(Key));
    CHECK(claims.CanApply(peer));
}
