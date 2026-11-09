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
    captures.Put({*config.Source(*World::PickupKey(memory, 0x1000, config)), 7, 2, 10, 100});
    memory.words.clear();
    REQUIRE(captures.Take(7, 2, 11) == Bit);
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
    pickups.Put({Bit, 7, 2, 10, 100});
    REQUIRE_FALSE(pickups.Take(8, 2, 11));
    REQUIRE(pickups.Take(7, 2, 11) == Bit);
    REQUIRE_FALSE(pickups.Take(7, 2, 12));
    pickups.Put({Bit, 7, 2, 10, 100});
    REQUIRE_FALSE(pickups.Take(7, 3, 11));
    REQUIRE_FALSE(pickups.Take(7, 2, 11));
    pickups.Put({Bit, 7, 2, 10, 100});
    REQUIRE_FALSE(pickups.Take(7, 2, 9));
    pickups.Put({Bit, 7, 2, 10, 100});
    REQUIRE_FALSE(pickups.Take(7, 2, 1811));
    pickups.Put({Bit, 7, 2, 10, 100});
    REQUIRE(pickups.Take(7, 2, 1810) == Bit);
    pickups.Put({Bit, 7, 2, 10, 100});
    pickups.Put({Origin{}, 7, 2, 11, 100});
    REQUIRE_FALSE(pickups.Take(7, 2, 12));
    for (u64 thread = 1; thread <= 65; ++thread) pickups.Put({Bit, thread, 2, 10, 100});
    REQUIRE_FALSE(pickups.Take(65, 2, 11));
    REQUIRE(pickups.Take(64, 2, 11) == Bit);
    pickups.Clear();
    REQUIRE_FALSE(pickups.Take(1, 2, 11));
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
