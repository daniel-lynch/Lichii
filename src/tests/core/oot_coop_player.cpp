// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstring>

#include <catch2/catch_test_macros.hpp>
#include "core/arm/dynarmic/oot_coop_player.h"

namespace {
namespace Player = Core::OotCoop::Player;
using Json = nlohmann::json;

struct Memory {
    std::array<u8, 4096> bytes{};
    bool IsValidVirtualAddressRange(u64 address, u64 size) const {
        return address && address <= bytes.size() && size <= bytes.size() - address;
    }
    u8 Read8(u64 a) const { return bytes.at(a); }
    u32 Read32(u64 a) const {
        u32 v{};
        std::memcpy(&v, bytes.data() + a, 4);
        return v;
    }
    u64 Read64(u64 a) const {
        u64 v{};
        std::memcpy(&v, bytes.data() + a, 8);
        return v;
    }
    void Put(u64 a, u64 v, size_t n = 8) { std::memcpy(bytes.data() + a, &v, n); }
    void Text(u64 a, const char* s) { std::memcpy(bytes.data() + a, s, std::strlen(s) + 1); }
};

// Synthetic layout: cell -> manager -> user -> owner actor, holder -> user, record backlink.
constexpr u64 Base = 256, Manager = 512, User = 768, Actor = 1024, Holder = 1280, Record = 1536, NameAt = 1792;
Json KeysJson() {
    return {{"user_cell", "0x8"}, {"user_equipment", 16}, {"owner", 24}, {"holder", 32},
            {"equipment", 40}, {"actor_record", 48}, {"actor_id", 56}, {"record_actor", 8},
            {"record_id", 16}, {"actor_name", 64}, {"lifecycle", 72}, {"lifecycle_lo", 3},
            {"lifecycle_hi", 5}, {"names", {"SyntheticPlayer"}}};
}
Memory World() {
    Memory m;
    m.Put(Base + 8, Manager);
    m.Put(Manager + 16, User);
    m.Put(User + 24, Actor);
    m.Put(Actor + 32, Holder);
    m.Put(Holder + 40, User);
    m.Put(Actor + 48, Record);
    m.Put(Actor + 56, 9, 4);
    m.Put(Record + 8, Actor);
    m.Put(Record + 16, 9, 4);
    m.Put(Actor + 64, NameAt);
    m.Text(NameAt, "SyntheticPlayer");
    m.Put(Actor + 72, 1, 4);
    return m;
}
} // namespace

TEST_CASE("OoT co-op resident player resolves by reads and validates every link", "[core][oot_coop]") {
    const auto keys = Player::DecodeKeys(KeysJson());
    REQUIRE(keys);
    auto m = World();
    const auto found = Player::Find(m, Base, *keys);
    REQUIRE(found);
    REQUIRE(*found == Player::Resident{Actor, Record, User, 9});
    REQUIRE(Player::Validate(m, Base, *found, *keys));
    SECTION("teardown lifecycle") {
        m.Put(Actor + 72, 4, 4);
        REQUIRE_FALSE(Player::Find(m, Base, *keys));
        REQUIRE_FALSE(Player::Validate(m, Base, *found, *keys));
    }
    SECTION("new generation") {
        m.Put(Record + 16, 10, 4);
        REQUIRE_FALSE(Player::Validate(m, Base, *found, *keys));
        m.Put(Actor + 56, 10, 4);
        REQUIRE(Player::Find(m, Base, *keys)->generation == 10); // a fresh find adopts it
        REQUIRE_FALSE(Player::Validate(m, Base, *found, *keys));
    }
    SECTION("holder names another user") {
        m.Put(Holder + 40, User + 8);
        REQUIRE_FALSE(Player::Find(m, Base, *keys));
    }
    SECTION("another registered user") {
        m.Put(Manager + 16, User + 64);
        REQUIRE_FALSE(Player::Validate(m, Base, *found, *keys));
    }
    SECTION("wrong actor name") {
        m.Text(NameAt, "SyntheticOther");
        REQUIRE_FALSE(Player::Find(m, Base, *keys));
    }
    SECTION("invalid generation sentinel") {
        m.Put(Actor + 56, 0xFFFFFFFF, 4);
        m.Put(Record + 16, 0xFFFFFFFF, 4);
        REQUIRE_FALSE(Player::Find(m, Base, *keys));
    }
    SECTION("unmapped chain") {
        m.Put(Base + 8, 0x7FFFFFFF);
        REQUIRE_FALSE(Player::Find(m, Base, *keys));
    }
}

TEST_CASE("OoT co-op resident player keys are strict", "[core][oot_coop]") {
    REQUIRE(Player::DecodeKeys(KeysJson()));
    auto bad = KeysJson();
    bad["unexpected"] = 1;
    REQUIRE_FALSE(Player::DecodeKeys(bad));
    bad = KeysJson();
    bad.erase("holder");
    REQUIRE_FALSE(Player::DecodeKeys(bad));
    bad = KeysJson();
    bad["names"] = Json::array();
    REQUIRE_FALSE(Player::DecodeKeys(bad));
    bad = KeysJson();
    bad["lifecycle_lo"] = 6;
    REQUIRE_FALSE(Player::DecodeKeys(bad));
    bad = KeysJson();
    bad["owner"] = -1;
    REQUIRE_FALSE(Player::DecodeKeys(bad));
    bad = KeysJson();
    bad["user_cell"] = "0x10000001";
    REQUIRE_FALSE(Player::DecodeKeys(bad));
}
