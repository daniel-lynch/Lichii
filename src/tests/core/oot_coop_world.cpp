// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <limits>
#include <map>
#include <string>
#include <catch2/catch_test_macros.hpp>
#include "core/arm/dynarmic/oot_coop_runtime.h"
#include "core/arm/dynarmic/oot_coop_sync.h"
#include "core/arm/dynarmic/oot_coop_world.h"
#include "tests/core/oot_test_profile.h"

namespace {
namespace Coop = Core::OotCoop;
namespace World = Coop::World;
namespace Trace = Core::OotGameDataTrace;
using Json = nlohmann::json;
using World::Origin;
using World::OriginKind;
constexpr u64 Key = 0xA123456789ABCDEF;
constexpr u64 Partner = 0xB23456789ABCDEF1;
constexpr Origin Bit{OriginKind::Bit, Key, 0};
constexpr Origin Token{OriginKind::Token, Key, Partner};

Coop::Change Give(Origin source, std::string sender = "peer", u64 seq = 1) {
    Coop::Change change{Coop::Kind::Item, std::move(sender), seq,
                        Coop::PackItem({3, 0, {0, 0, 0, 0}}), 1};
    change.name = "Test_Collectible";
    change.source = source;
    return change;
}

struct Registers {
    std::array<u64, 31> values{};
    u64 GetRegister(size_t index) const { return values.at(index); }
    void SetRegister(size_t index, u64 value) { values.at(index) = value; }
};

struct Memory {
    std::map<u64, u64> words;
    std::map<u64, u32> instructions;
    size_t reads{};
    bool IsValidVirtualAddressRange(u64 address, size_t size) const {
        return size == sizeof(u64) && words.contains(address);
    }
    u64 Read64(u64 address) { ++reads; return words.at(address); }
    u32 Read32(u64 address) const { return instructions.at(address); }
};

Json ProfileFields() {
    return {{"manager_cell", "0x200000"},
            {"pickup", {{"callers", {"0x120004", "0x130004"}},
                        {"names", {"Test_Collectible"}},
                        {"component_entry", {{"at", "0x110000"}, {"words", {"0xD503237F"}}}},
                        {"actor_off", "0x20"}, {"metadata_off", "0x40"}, {"key_off", "0x60"},
                        {"death_wrapper", {{"at", "0x140000"}, {"words", {1}}}},
                        {"return_sites", Json::array({
                            {{"at", "0x120004"}, {"words", {"0x90000008"}}},
                            {{"at", "0x130004"}, {"words", {"0xB0000028"}}}})}}},
            {"wallet", {{"shop_lr", "0x150004"}, {"capacity_hash", 103}, {"pending_off", "0x80"}}},
            {"token", {{"enabled", false}, {"counter_hash", 104}, {"ownership_hash", 105},
                       {"pairs", Json::array({Json::array({Key, Partner})})}}},
            {"world", {{"hearts", false}, {"cleanup", "flag_only"}, {"wallet_reconcile", "off"},
                       {"families", Json::array({{{"key", Key}, {"family", "token-H"}},
                                                {{"key", Partner}, {"family", "token-partner"}}})}}}};
}

std::optional<Trace::Profile> DecodeProfile(const Json& fields) {
    return Coop::DecodeProfile(Json{{"offsets", {{OotTestProfile::BuildId, fields}}}},
                               OotTestProfile::BuildId);
}
} // namespace

TEST_CASE("OoT world origin codec retains u64 identity and chest compatibility", "[core][oot_coop]") {
    const std::array sources{Origin{}, Origin::Chest(0xABCDEF01), Bit, Token,
                            Origin{OriginKind::Bit, UINT64_MAX, 0}};
    for (const auto source : sources) {
        const auto change = Give(source);
        const auto encoded = Coop::EncodeChange(change);
        REQUIRE(encoded.at("origin") == "peer");
        REQUIRE(World::DecodeOrigin(encoded) == source);
        const auto packet = Coop::DecodeLine(encoded.dump());
        REQUIRE(packet);
        REQUIRE(Coop::DecodeChange(*packet) == change);
        if (source.kind == OriginKind::Chest) {
            REQUIRE(encoded.at("origin_key") == source.key);
            auto legacy = encoded;
            legacy.erase("origin_kind");
            REQUIRE(Coop::DecodeChange(legacy) == change);
        } else {
            REQUIRE_FALSE(encoded.contains("origin_key"));
        }
    }
    Coop::Change flag{Coop::Kind::Bit, "peer", 2, Partner, 1};
    flag.source = Token;
    REQUIRE(Coop::DecodeChange(Coop::EncodeChange(flag)) == flag);
    flag.key = 99;
    REQUIRE_FALSE(Coop::DecodeChange(Coop::EncodeChange(flag)));
    flag.key = Key;
    flag.source = Origin::Chest(101);
    REQUIRE_FALSE(Coop::DecodeChange(Coop::EncodeChange(flag)));
}

TEST_CASE("OoT world origin decoder rejects ambiguous and overflowing identities", "[core][oot_coop]") {
    const auto valid = Coop::EncodeChange(Give(Token));
    for (const auto& invalid : {Json(-1), Json(0), Json(true), Json(1.25),
                               Json("0xA123456789ABCDEF"), Json(1.0e30)}) {
        auto packet = valid;
        packet["origin_key64"] = invalid;
        REQUIRE_FALSE(Coop::DecodeChange(packet));
        packet = valid;
        packet["origin_partner"] = invalid;
        REQUIRE_FALSE(Coop::DecodeChange(packet));
    }
    for (const auto* field : {"origin_kind", "origin_key64", "origin_partner"}) {
        auto packet = valid;
        packet.erase(field);
        REQUIRE_FALSE(Coop::DecodeChange(packet));
    }
    auto packet = valid;
    packet["origin_partner"] = Key;
    REQUIRE_FALSE(Coop::DecodeChange(packet));
    packet = valid;
    packet["origin_key"] = 101;
    REQUIRE_FALSE(Coop::DecodeChange(packet));
    for (const auto& kind : {Json("none"), Json("bit"), Json("other"), Json(1), Json(true)}) {
        packet = valid;
        packet["origin_kind"] = kind;
        REQUIRE_FALSE(Coop::DecodeChange(packet));
    }
    packet = Coop::EncodeChange(Give(Origin::Chest(101)));
    packet["origin_key"] = u64{1} << 32;
    REQUIRE_FALSE(Coop::DecodeChange(packet));
    REQUIRE_FALSE(World::Valid({OriginKind::None, 1, 0}));
    REQUIRE_FALSE(World::Valid({OriginKind::Bit, 1, 2}));
    REQUIRE_FALSE(World::Number("0x10000000000000000", UINT64_MAX));
    REQUIRE_FALSE(World::Number("0x12tail", UINT64_MAX));
    REQUIRE_FALSE(World::Number("123", UINT64_MAX));
    REQUIRE(World::Number("0xFFFFFFFFFFFFFFFF", UINT64_MAX) == UINT64_MAX);
}

TEST_CASE("OoT world claims cover both winners in the three give orders", "[core][oot_coop]") {
    for (const auto source : {Origin::Chest(101), Bit, Token}) {
        for (const auto& winner : {std::string("local"), std::string("peer")}) {
            const std::string loser = winner == "local" ? "peer" : "local";
            {
                World::Claims claims;
                REQUIRE(claims.Reserve(source, winner));
                REQUIRE(claims.Complete(source, winner));
                REQUIRE_FALSE(claims.Reserve(source, loser));
                REQUIRE(claims.Applied(source));
            }
            {
                World::Claims claims;
                REQUIRE(claims.Reserve(source, winner));
                REQUIRE_FALSE(claims.Reserve(source, loser));
                REQUIRE_FALSE(claims.Complete(source, loser));
                REQUIRE_FALSE(claims.Applied(source));
                REQUIRE(claims.Complete(source, winner));
            }
            {
                World::Claims claims;
                REQUIRE(claims.Reserve(source, winner));
                claims.Cancel(source, loser);
                REQUIRE(claims.Busy(source));
                REQUIRE_FALSE(claims.Reserve(source, loser));
                REQUIRE(claims.Complete(source, winner));
                claims.Cancel(source, winner);
                REQUIRE(claims.Applied(source));
                REQUIRE_FALSE(claims.Reserve(source, loser));
            }
        }
    }
}

TEST_CASE("OoT world cancellation and token flags retain the successful receipt", "[core][oot_coop]") {
    World::Claims claims;
    REQUIRE(claims.Reserve(Token, "local"));
    REQUIRE_FALSE(claims.GaveFlag(Key));
    claims.Cancel(Token, "local");
    REQUIRE_FALSE(claims.Busy(Token));
    REQUIRE(claims.Reserve(Token, "peer"));
    REQUIRE(claims.Complete(Token, "peer"));
    REQUIRE(claims.GaveFlag(Key) == Token);
    REQUIRE(claims.GaveFlag(Partner) == Token);
    REQUIRE_FALSE(claims.GaveFlag(99));
    REQUIRE(World::Flags(Token) == std::vector<u64>{Key, Partner});
    REQUIRE(World::Flags(Bit) == std::vector<u64>{Key});
    REQUIRE(World::Flags(Origin::Chest(101)).empty());
    claims.Clear();
    REQUIRE_FALSE(claims.Applied(Token));
    REQUIRE_FALSE(claims.GaveFlag(Key));
    REQUIRE(claims.Reserve(Bit, "peer"));
    REQUIRE(claims.Complete(Bit, "peer"));
    REQUIRE_FALSE(claims.GaveFlag(0));
    const Origin invalid_none{OriginKind::None, 101, 0};
    REQUIRE_FALSE(claims.Reserve(invalid_none, "peer"));
    REQUIRE_FALSE(claims.Complete(invalid_none, "peer"));
}

TEST_CASE("OoT world origin receipts complement sender sequence deduplication", "[core][oot_coop]") {
    Coop::State state;
    World::Claims claims;
    const auto first = Give(Bit, "peer", 1);
    REQUIRE(state.Merge(first));
    REQUIRE(claims.Reserve(first.source, first.origin));
    REQUIRE(claims.Complete(first.source, first.origin));
    REQUIRE_FALSE(state.Merge(first));
    const auto second = Give(Bit, "peer", 2);
    REQUIRE(state.Merge(second));
    REQUIRE_FALSE(claims.Reserve(second.source, second.origin));
    const auto other_sender = Give(Bit, "other", 1);
    REQUIRE(state.Merge(other_sender));
    REQUIRE_FALSE(claims.Reserve(other_sender.source, other_sender.origin));
    REQUIRE(state.Snapshot().empty());

    Coop::State baseline;
    Coop::Change flag{Coop::Kind::Bit, "peer", 3, Key, 1};
    flag.source = Bit;
    REQUIRE(baseline.Merge(flag));
    REQUIRE(baseline.Snapshot().front().source == Origin{});
    World::Claims fresh;
    REQUIRE_FALSE(fresh.Applied(Bit));
    REQUIRE(fresh.Reserve(Bit, "peer"));
    REQUIRE_FALSE(fresh.Reserve(Origin{OriginKind::Chest, u64{1} << 32, 0}, "peer"));
}

TEST_CASE("OoT world profile gates families and validates explicit token pairs", "[core][oot_coop]") {
    auto fields = ProfileFields();
    auto profile = DecodeProfile(fields);
    REQUIRE(profile);
    REQUIRE(profile->world.AdmitsFlag(Key));
    REQUIRE(profile->world.AdmitsFlag(Partner));
    REQUIRE_FALSE(profile->world.Source(Key));
    fields["token"]["enabled"] = true;
    profile = DecodeProfile(fields);
    REQUIRE(profile);
    REQUIRE(profile->world.Source(Key) == Token);
    REQUIRE(profile->world.Source(Partner) == Token);
    REQUIRE(profile->world.Admits(Token));
    REQUIRE_FALSE(profile->world.Admits(Bit));
    REQUIRE_FALSE(profile->world.Admits({OriginKind::Token, Partner, Key}));
    REQUIRE(profile->world.capacity_hash == 103);
    REQUIRE(profile->pickup_returns.size() == 2);
    for (const auto& family : {"heart", "piece", "container"}) {
        World::Config config;
        REQUIRE(World::DecodeConfig(Json{{"families", Json::array({{{"key", Key}, {"family", family}}})}}, config));
        REQUIRE_FALSE(config.AdmitsFlag(Key));
        config.hearts = true;
        REQUIRE(config.Source(Key) == Bit);
    }
    auto malformed = fields;
    malformed["token"]["pairs"] = Json::array({Json::array({Key, Key})});
    REQUIRE_FALSE(DecodeProfile(malformed));
    malformed = fields;
    malformed["token"]["pairs"].push_back(Json::array({Partner, 99}));
    REQUIRE_FALSE(DecodeProfile(malformed));
    malformed = fields;
    malformed["world"]["families"][1]["family"] = "rupee";
    REQUIRE_FALSE(DecodeProfile(malformed));
    malformed = fields;
    malformed["pickup"]["return_sites"][0]["words"][0] = "0x90000009";
    REQUIRE_FALSE(DecodeProfile(malformed));
    malformed = fields;
    malformed["pickup"]["metadata_off"] = "0x41";
    REQUIRE_FALSE(DecodeProfile(malformed));
    malformed = fields;
    malformed["world"]["cleanup"] = "guess";
    REQUIRE_FALSE(DecodeProfile(malformed));
    malformed = fields;
    malformed["world"]["families"].push_back(malformed["world"]["families"][0]);
    REQUIRE_FALSE(DecodeProfile(malformed));
    malformed = fields;
    malformed["pickup"].erase("component_entry");
    malformed["pickup"].erase("return_sites");
    REQUIRE_FALSE(DecodeProfile(malformed));
}

TEST_CASE("OoT world pickup identity validates each synthetic pointer and retains only keys", "[core][oot_coop]") {
    World::Config config;
    config.actor_off = 0x20;
    config.metadata_off = 0x40;
    config.key_off = 0x60;
    config.families.emplace(Key, World::Family::Rupee);
    Memory memory;
    memory.words = {{0x1020, 0x2000}, {0x2040, 0x3000}, {0x3060, Key}};
    REQUIRE(World::PickupKey(memory, 0x1000, config) == Key);
    REQUIRE(memory.reads == 3);
    World::Pickups captures;
    captures.Put({*config.Source(*World::PickupKey(memory, 0x1000, config)), 7, 2, 10, 0x8000});
    memory.words.clear();
    REQUIRE(captures.Take(7, 2, 11, 0x7F00) == Bit);
    REQUIRE_FALSE(World::PickupKey(memory, 0x1000, config));
    REQUIRE_FALSE(World::PickupKey(memory, UINT64_MAX - 8, config));
    REQUIRE_FALSE(World::PickupKey(memory, 0, config));
    memory.words = {{0x1020, 0x2000}, {0x2040, 0}, {0x3060, Key}};
    REQUIRE_FALSE(World::PickupKey(memory, 0x1000, config));
    memory.words[0x2040] = 0x3000;
    memory.words[0x3060] = 99;
    REQUIRE_FALSE(World::PickupKey(memory, 0x1000, config));
    memory.words[0x3060] = Key;
    config.key_off = 0;
    REQUIRE_FALSE(World::PickupKey(memory, 0x1000, config));
}

TEST_CASE("OoT world pickup contexts are thread epoch and age bounded", "[core][oot_coop]") {
    World::Pickups pickups;
    pickups.Put({Bit, 7, 2, 10, 0x8000});
    REQUIRE_FALSE(pickups.Take(8, 2, 11, 0x7F00));
    REQUIRE(pickups.Take(7, 2, 11, 0x7F00) == Bit);
    REQUIRE_FALSE(pickups.Take(7, 2, 12, 0x7F00));
    pickups.Put({Bit, 7, 2, 10, 0x8000});
    REQUIRE_FALSE(pickups.Take(7, 3, 11, 0x7F00));
    REQUIRE_FALSE(pickups.Take(7, 2, 11, 0x7F00));
    pickups.Put({Bit, 7, 2, 10, 0x8000});
    REQUIRE_FALSE(pickups.Take(7, 2, 9, 0x7F00));
    pickups.Put({Bit, 7, 2, 10, 0x8000});
    REQUIRE_FALSE(pickups.Take(7, 2, 1811, 0x7F00));
    pickups.Put({Bit, 7, 2, 10, 0x8000});
    REQUIRE(pickups.Take(7, 2, 1810, 0x7F00) == Bit);
    pickups.Put({Bit, 7, 2, 10, 0x8000});
    pickups.Put({Origin{}, 7, 2, 11, 0x8000});
    REQUIRE_FALSE(pickups.Take(7, 2, 12, 0x7F00));
    for (u64 thread = 1; thread <= 65; ++thread) pickups.Put({Bit, thread, 2, 10, 0x8000});
    REQUIRE_FALSE(pickups.Take(65, 2, 11, 0x7F00));
    REQUIRE(pickups.Take(64, 2, 11, 0x7F00) == Bit);
    pickups.Clear();
    REQUIRE_FALSE(pickups.Take(1, 2, 11, 0x7F00));
    // The give must run nested in the component invocation: below its entry stack, not too deep.
    for (const u64 sp : {u64{0x8000}, u64{0x8010}, u64{0x8000 - World::Pickups::StackSpan - 0x10}}) {
        pickups.Put({Bit, 7, 2, 10, 0x8000});
        REQUIRE_FALSE(pickups.Take(7, 2, 11, sp));
    }
    pickups.Put({Bit, 7, 2, 10, 0x8000});
    REQUIRE(pickups.Take(7, 2, 11, 0x8000 - World::Pickups::StackSpan) == Bit);
}

TEST_CASE("OoT world wallet caller matrix preserves signed deltas and suppresses application echoes", "[core][oot_coop]") {
    constexpr u64 base = 0x100000000, reward = 0x100004, shop = 0x110004;
    constexpr u64 sequence = 0x120004, sentinel = base + 0x130004;
    Registers jit;
    Trace::Profile profile;
    profile.wallet_hash = 101;
    profile.rupee_callers = {reward};
    profile.world.shop_lr = shop;
    profile.give_deny_callers = {sequence};
    const auto call = [&](u64 lr, s32 delta, bool applying = false, bool enabled = true) {
        jit.SetRegister(30, lr);
        jit.SetRegister(1, static_cast<u32>(delta));
        jit.SetRegister(2, 7);
        return Coop::ReadRupeeCall(jit, base, profile, sentinel, enabled, applying);
    };
    for (const u64 caller : {reward, shop}) {
        for (const s32 delta : {-999, -5, 1, 999}) {
            const auto observed = call(base + caller, delta);
            REQUIRE(observed.forward);
            REQUIRE(observed.change.value == delta);
            REQUIRE(observed.change.key == 101);
            REQUIRE_FALSE(observed.replay);
            REQUIRE_FALSE(call(base + caller, delta, true).forward);
            REQUIRE(call(base + caller, delta, true).replay);
        }
        for (const s32 delta : {-1000, 0, 1000, std::numeric_limits<s32>::min()})
            REQUIRE_FALSE(call(base + caller, delta).forward);
    }
    REQUIRE_FALSE(call(base + sequence, 5).forward);
    REQUIRE_FALSE(call(base + 0x140004, -5).forward);
    REQUIRE_FALSE(call(base - 4, 5).forward);
    REQUIRE_FALSE(call(base + reward, 5, false, false).forward);
    REQUIRE_FALSE(call(sentinel, -5).forward);
    REQUIRE(call(sentinel, -5).replay);
    profile.wallet_hash = 0;
    REQUIRE_FALSE(call(base + reward, 5).forward);
}

TEST_CASE("OoT world wallet baseline codec and reconciliation reject ambiguous corrections", "[core][oot_coop]") {
    using World::Wallet;
    using World::Reconcile;
    const Wallet ours{20, 100}, peer{35, 100};
    REQUIRE(World::DecodeWallet(World::EncodeWallet(ours)) == ours);
    REQUIRE_FALSE(World::ReconcileDelta(ours, peer, Reconcile::Off));
    REQUIRE(World::ReconcileDelta(ours, peer, Reconcile::TargetCurrent) == 15);
    REQUIRE(World::ReconcileDelta(peer, ours, Reconcile::TargetCurrent) == -15);
    REQUIRE(World::ReconcileDelta(ours, ours, Reconcile::TargetCurrent) == 0);
    REQUIRE_FALSE(World::ReconcileDelta(ours, {35, 200}, Reconcile::TargetCurrent));
    REQUIRE_FALSE(World::ReconcileDelta({-1, 100}, peer, Reconcile::TargetCurrent));
    REQUIRE_FALSE(World::ReconcileDelta(ours, {101, 100}, Reconcile::TargetCurrent));
    REQUIRE_FALSE(World::ReconcileDelta({0, 2000}, {1000, 2000}, Reconcile::TargetCurrent));
    for (const auto& invalid : {Json(-1), Json(true), Json("20"), Json(1.5), Json(u64{1} << 32)}) {
        auto packet = World::EncodeWallet(ours);
        packet["balance"] = invalid;
        REQUIRE_FALSE(World::DecodeWallet(packet));
    }
    REQUIRE_FALSE(World::DecodeWallet(Json{{"balance", 101}, {"capacity", 100}}));
    REQUIRE_FALSE(World::DecodeWallet(Json{{"balance", 20}, {"capacity", 100}, {"extra", 0}}));
    REQUIRE_FALSE(World::DecodeWallet(Json{{"balance", 20}}));
}

TEST_CASE("OoT world pickup return resumes ADRP x8 across page and sign boundaries", "[core][oot_coop]") {
    Registers jit;
    Memory memory;
    for (const u64 pc : {u64{0x10000FFC}, u64{0x123456789ABC}}) {
        for (const s64 pages : {-1048576LL, -5LL, -1LL, 0LL, 1LL, 5LL, 1048575LL}) {
            const u32 imm = static_cast<u32>(pages) & 0x1FFFFF;
            const u32 word = 0x90000008 | ((imm & 3) << 29) | ((imm >> 2) << 5);
            memory.instructions[pc] = word;
            jit.SetRegister(8, UINT64_MAX);
            jit.SetRegister(0, 123);
            Trace::Resume(Trace::Point::PickupReturn, jit, memory, pc);
            const u64 expected = (pc & ~u64{0xFFF}) + static_cast<u64>(pages * 4096);
            REQUIRE(jit.GetRegister(8) == expected);
            REQUIRE(jit.GetRegister(0) == 123);
        }
    }
}

TEST_CASE("OoT world typed chest claims keep ambiguous events unlinked", "[core][oot_coop]") {
    Coop::ChestClaims claims;
    claims.LocalOpen(Origin::Chest(101), 1);
    claims.LocalOpen(Origin::Chest(102), 1);
    claims.LocalOpen(Origin::Chest(103), 1);
    REQUIRE_FALSE(claims.End(1));
}

namespace {
// Byte-addressed synthetic guest memory for the cleanup resolver (little endian).
struct Bytes {
    std::map<u64, u8> bytes;
    void W64(u64 a, u64 v) { for (int i = 0; i < 8; ++i) bytes[a + i] = static_cast<u8>(v >> (8 * i)); }
    void W32(u64 a, u32 v) { for (int i = 0; i < 4; ++i) bytes[a + i] = static_cast<u8>(v >> (8 * i)); }
    bool IsValidVirtualAddressRange(u64 a, size_t size) const {
        for (size_t i = 0; i < size; ++i) if (!bytes.contains(a + i)) return false;
        return true;
    }
    u64 Read64(u64 a) const { u64 v{}; for (int i = 7; i >= 0; --i) v = v << 8 | bytes.at(a + i); return v; }
    u32 Read32(u64 a) const { u32 v{}; for (int i = 3; i >= 0; --i) v = v << 8 | bytes.at(a + i); return v; }
};

Json ResolverJson() {
    return {{"root_cell", "0x1000"},
            {"groups", Json::array({Json::array({"0x10", "0x18"}), Json::array({"0x20", "0x28"})})},
            {"block", {{"stride", "0x30"}, {"capacity", "0x0"}, {"records", "0x8"}, {"used", "0x14"}}},
            {"record", {{"stride", "0x80"}, {"flags", "0x40"}, {"in_use", "0x10"}, {"key", "0x48"},
                        {"keyed_flags", "0x4"}, {"keyed", "0x8"}, {"actor", "0x20"}}},
            {"list", {{"head", "0x38"}, {"sentinel", "0x30"}, {"next", "0x8"}, {"back", "0x80"}}},
            {"actor", {{"record", "0x50"}, {"lifecycle", "0x4"}, {"live", Json::array({2, 6})},
                       {"refs", "0x58"}, {"flags", "0x8"}, {"deleting", "0x10000"},
                       {"link", Json::array({"0x60", "0x10", "0x8", "0x10"})}}},
            {"death", {{"chain", Json::array({"0x68", "0x18"})}, {"actor", "0x8"}, {"state", "0x10"},
                       {"flags", "0x14"}, {"disabled", "0x20"}, {"reason", 4}}}};
}

// One manager at base+0x1000 -> 0x10000; group A: one block of two records; the second holds Key.
struct World3 {
    static constexpr u64 Base = 0x100000, Manager = 0x10000, Blocks = 0x11000, Records = 0x12000;
    static constexpr u64 Actor = 0x14000, LinkRecord = 0x15000, Table = 0x16000, Death = 0x17000;
    Bytes m;
    World3() {
        m.W64(Base + 0x1000, Manager);
        m.W32(Manager + 0x10, 1); m.W64(Manager + 0x18, Blocks);
        m.W32(Manager + 0x20, 0); m.W64(Manager + 0x28, 0);
        m.W64(Manager + 0x38, Manager + 0x30); // empty list
        m.W32(Blocks, 4); m.W64(Blocks + 8, Records); m.W32(Blocks + 0x14, 2);
        for (u64 i = 0; i < 2; ++i) {
            const u64 rec = Records + i * 0x80;
            m.W32(rec + 0x4, 0x8); m.W32(rec + 0x40, 0x10); m.W64(rec + 0x48, i ? Key : 99);
            m.W64(rec + 0x20, i ? Actor : 0);
        }
        m.W64(Actor + 0x50, Records + 0x80); m.W32(Actor + 0x4, 4); m.W32(Actor + 0x58, 1);
        m.W32(Actor + 0x8, 0); m.W64(Actor + 0x60, LinkRecord); m.W32(Actor + 0x10, 77);
        m.W64(LinkRecord + 0x8, Actor); m.W32(LinkRecord + 0x10, 77);
        m.W64(Actor + 0x68, Table); m.W64(Table + 0x18, Death);
        m.W64(Death + 0x8, Actor); m.W32(Death + 0x10, 0); m.W32(Death + 0x14, 0);
    }
};
} // namespace

TEST_CASE("OoT world cleanup resolver finds the live actor by key with pure reads", "[core][oot_coop]") {
    World::Resolver r;
    REQUIRE(World::DecodeResolver(ResolverJson(), r));
    World3 w;
    auto walk = World::Find(w.m, World3::Base, r, Key);
    REQUIRE(walk.targets.size() == 1);
    REQUIRE(walk.targets[0].record == World3::Records + 0x80);
    REQUIRE(walk.targets[0].actor == World3::Actor);
    REQUIRE(walk.targets[0].Callable());
    REQUIRE(walk.records == 2);
    REQUIRE_FALSE(walk.bounded);
    REQUIRE(World::Find(w.m, World3::Base, r, 12345).targets.empty());
    // Each failed check names itself; none is callable.
    const auto state = [&](auto edit) {
        World3 x;
        edit(x.m);
        const auto found = World::Find(x.m, World3::Base, r, Key);
        return found.targets.empty() ? std::string{"none"} : std::string{found.targets[0].state};
    };
    REQUIRE(state([](Bytes& m) { m.W32(World3::Records + 0x80 + 0x40, 0); }) == "none"); // not in use
    REQUIRE(state([](Bytes& m) { m.W64(World3::Records + 0x80 + 0x20, 0); }) == "unloaded");
    REQUIRE(state([](Bytes& m) { m.W64(World3::Records + 0x80 + 0x20, World3::Actor | 1); }) == "detached");
    REQUIRE(state([](Bytes& m) { m.W64(World3::Actor + 0x50, 0); }) == "back link");
    REQUIRE(state([](Bytes& m) { m.W32(World3::Actor + 0x4, 7); }) == "lifecycle");
    REQUIRE(state([](Bytes& m) { m.W32(World3::Actor + 0x58, 0xFFFFFFFF); }) == "destroying");
    REQUIRE(state([](Bytes& m) { m.W32(World3::LinkRecord + 0x10, 78); }) == "link");
    REQUIRE(state([](Bytes& m) { m.W32(World3::Actor + 0x8, 0x10000); }) == "delete requested");
    REQUIRE(state([](Bytes& m) { m.W64(World3::Death + 0x8, 0); }) == "death component");
    REQUIRE(state([](Bytes& m) { m.W32(World3::Death + 0x10, 0x41); }) == "death pending");
    REQUIRE(state([](Bytes& m) { m.W32(World3::Death + 0x14, 0x20); }) == "death disabled");
    REQUIRE(state([](Bytes& m) { m.W32(World3::Records + 0x80 + 0x4, 0); }) == "no key flag");
    // Bounds: an implausible block count or used > capacity is skipped and reported.
    World3 big;
    big.m.W32(World3::Manager + 0x10, 65);
    REQUIRE(World::Find(big.m, World3::Base, r, Key).bounded);
    World3 used;
    used.m.W32(World3::Blocks + 0x14, 5);
    walk = World::Find(used.m, World3::Base, r, Key);
    REQUIRE(walk.bounded);
    REQUIRE(walk.targets.empty());
    // The list is walked only when the arrays have no match: a record reached through node - back.
    World3 listed;
    listed.m.W64(World3::Records + 0x80 + 0x48, 5);             // the array no longer holds Key
    const u64 rec = 0x18000, node = rec + 0x80;
    listed.m.W32(rec + 0x4, 0x8); listed.m.W32(rec + 0x40, 0x10); listed.m.W64(rec + 0x48, Key);
    listed.m.W64(rec + 0x20, 0);
    listed.m.W64(World3::Manager + 0x38, node); listed.m.W64(node + 0x8, World3::Manager + 0x30);
    walk = World::Find(listed.m, World3::Base, r, Key);
    REQUIRE(walk.list == 1);
    REQUIRE(walk.targets.size() == 1);
    REQUIRE(walk.targets[0].state == "unloaded");
    listed.m.W64(node + 0x8, node); // a cycle stops at the cap
    REQUIRE(World::Find(listed.m, World3::Base, r, Key).bounded);
}

TEST_CASE("OoT world cleanup profile is strict and gated by family and mode", "[core][oot_coop]") {
    World::Resolver r;
    REQUIRE(World::DecodeResolver(ResolverJson(), r));
    for (const char* drop : {"root_cell", "groups", "block", "record", "actor", "death"}) {
        auto bad = ResolverJson();
        bad.erase(drop);
        World::Resolver x;
        REQUIRE_FALSE(World::DecodeResolver(bad, x));
    }
    auto bad = ResolverJson();
    bad["record"]["extra"] = 1;
    World::Resolver x;
    REQUIRE_FALSE(World::DecodeResolver(bad, x));
    bad = ResolverJson();
    bad["death"]["reason"] = 16;
    REQUIRE_FALSE(World::DecodeResolver(bad, x = {}));
    bad = ResolverJson();
    bad["actor"]["live"] = Json::array({6, 2});
    REQUIRE_FALSE(World::DecodeResolver(bad, x = {}));
    bad = ResolverJson();
    bad.erase("list");
    REQUIRE(World::DecodeResolver(bad, x = {}));
    REQUIRE(x.list_head == 0);

    World::Config config;
    config.families = {{Key, World::Family::Rupee}, {Partner, World::Family::TokenH}, {7, World::Family::Heart}};
    REQUIRE_FALSE(config.Cleans(Key)); // no resolver, flag_only
    config.resolver = r;
    REQUIRE_FALSE(config.Cleans(Key)); // flag_only
    config.cleanup = World::Cleanup::Death;
    REQUIRE(config.Cleans(Key));
    REQUIRE_FALSE(config.Cleans(Partner)); // tokens wait for their own death signal: never
    REQUIRE_FALSE(config.Cleans(7));
    config.hearts = true;
    REQUIRE(config.Cleans(7));
    REQUIRE_FALSE(config.Cleans(99));

    auto fields = ProfileFields();
    fields["cleanup"] = ResolverJson();
    REQUIRE(DecodeProfile(fields));
    fields["pickup"].erase("death_wrapper");
    REQUIRE_FALSE(DecodeProfile(fields));
}
