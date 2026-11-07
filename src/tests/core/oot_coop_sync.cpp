// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <limits>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "core/arm/dynarmic/oot_coop_sync.h"

namespace {
namespace Coop = Core::OotCoop;
namespace Trace = Core::OotGameDataTrace;
using Coop::Change;
using Coop::Kind;
using Json = nlohmann::json;
} // namespace

TEST_CASE("OoT co-op change codecs preserve opaque hashes", "[core][oot_coop]") {
    const std::array changes{
        Change{Kind::Bool, "a", 1, 101, 1, 0},
        Change{Kind::Int, "a", 2, 102, -15, 0},
        Change{Kind::Enum, "a", 3, 103, 19, 0},
        Change{Kind::Quest, "a", 4, 104, 7, 105},
        Change{Kind::Bit, "a", 5, std::numeric_limits<u64>::max(), 1, 0},
        Change{Kind::PouchAdd, "a", 6, 106, 3, 0},
    };
    for (const auto& change : changes) {
        const auto line = Coop::EncodeLine(Coop::EncodeChange(change));
        REQUIRE(line.back() == '\n');
        const auto packet = Coop::DecodeLine(line.substr(0, line.size() - 1));
        REQUIRE(packet);
        REQUIRE(Coop::DecodeChange(*packet) == change);
    }
    const Change total{Kind::PouchTotal, {}, 0, 107, 12, 0};
    REQUIRE_FALSE(Coop::DecodeChange(Coop::EncodeChange(total)));
    REQUIRE(Coop::DecodeChange(Coop::EncodeChange(total), true) == total);
}

TEST_CASE("OoT co-op rejects malformed or out of range wire values", "[core][oot_coop]") {
    REQUIRE_FALSE(Coop::DecodeLine("[1,2]"));
    REQUIRE_FALSE(Coop::DecodeLine("{"));
    REQUIRE_FALSE(Coop::DecodeLine(std::string(Coop::MaxFrame, ' ')));
    REQUIRE_FALSE(Coop::DecodeLine(std::string{"{\"x\":\"a\0b\"}", 11}));
    auto packet = Coop::EncodeChange({Kind::Bool, "a", 1, 101, 1, 0});
    packet["value"] = 1;
    REQUIRE_FALSE(Coop::DecodeChange(packet));
    packet["value"] = true;
    for (const auto& invalid : {Json(-1), Json(1.5), Json(true), Json(Coop::MaxSequence + 1)}) {
        packet["seq"] = invalid;
        REQUIRE_FALSE(Coop::DecodeChange(packet));
    }
    packet["seq"] = 1;
    packet["hash"] = u64{1} << 32;
    REQUIRE_FALSE(Coop::DecodeChange(packet));
    packet = Coop::EncodeChange({Kind::Quest, "a", 1, 101, 256, 102});
    REQUIRE_FALSE(Coop::DecodeChange(packet));
    packet = Coop::EncodeChange({Kind::Int, "a", 1, 101,
                                 s64{std::numeric_limits<s32>::max()} + 1, 0});
    REQUIRE_FALSE(Coop::DecodeChange(packet));
    packet = Coop::EncodeChange({Kind::PouchAdd, "a", 1, 101, 0, 0});
    REQUIRE_FALSE(Coop::DecodeChange(packet));
}

TEST_CASE("OoT co-op bool and actor bits only converge to true", "[core][oot_coop]") {
    Coop::State state;
    REQUIRE_FALSE(state.Merge({Kind::Bool, "a", 1, 101, 0, 0}));
    REQUIRE(state.Merge({Kind::Bool, "a", 2, 101, 1, 0}));
    REQUIRE_FALSE(state.Merge({Kind::Bool, "b", 1, 101, 0, 0}));
    REQUIRE_FALSE(state.Merge({Kind::Bool, "b", 2, 101, 1, 0}));
    REQUIRE(state.Find(Kind::Bool, 101)->value == 1);
    const u64 actor = std::numeric_limits<u64>::max();
    REQUIRE(state.Merge({Kind::Bit, "a", 3, actor, 1, 0}));
    REQUIRE_FALSE(state.Merge({Kind::Bit, "a", 4, actor, 0, 0}));
    REQUIRE(state.Find(Kind::Bit, actor)->value == 1);
}

TEST_CASE("OoT co-op quest ordering uses indices not hash order", "[core][oot_coop]") {
    Coop::State state;
    REQUIRE(state.Merge({Kind::Quest, "a", 1, 101, 5, 999}));
    REQUIRE_FALSE(state.Merge({Kind::Quest, "b", 1, 101, 4, 1000}));
    REQUIRE_FALSE(state.Merge({Kind::Quest, "b", 2, 101, 5, 1001}));
    REQUIRE(state.Merge({Kind::Quest, "b", 3, 101, 6, 1}));
    REQUIRE(state.Find(Kind::Quest, 101)->step == 1);
    REQUIRE(state.Find(Kind::Quest, 101)->value == 6);
    REQUIRE(state.Merge({Kind::Quest, "a", 2, 102, 0, 200}));
}

TEST_CASE("OoT co-op additive pouch dedup survives a replay window", "[core][oot_coop]") {
    Coop::State state;
    const Change first{Kind::PouchAdd, "a", 1, 101, 2, 0};
    REQUIRE(state.Merge(first));
    REQUIRE_FALSE(state.Merge(first));
    REQUIRE(state.Merge({Kind::PouchAdd, "b", 1, 101, 3, 0}));
    for (u64 seq = 2; seq < 600; ++seq) {
        REQUIRE(state.Merge({Kind::PouchAdd, "a", seq, 101, 1, 0}));
    }
    REQUIRE_FALSE(state.Merge(first));
    REQUIRE(state.Find(Kind::PouchTotal, 101)->value == 603);
    // Snapshot totals are absolute, not a second application of the accumulated grants.
    REQUIRE_FALSE(state.Merge({Kind::PouchTotal, {}, 0, 101, 603, 0}, false));
    REQUIRE(state.Merge({Kind::PouchTotal, {}, 0, 101, 610, 0}, false));
    REQUIRE(state.Find(Kind::PouchTotal, 101)->value == 610);
    REQUIRE_FALSE(state.Merge({Kind::PouchAdd, "a", 600, 101, -1, 0}));
}

TEST_CASE("OoT co-op scalar state and runtime deny-list", "[core][oot_coop]") {
    const auto denied = Coop::DecodeDenyList(Json::array({101, 102}));
    REQUIRE(denied);
    REQUIRE_FALSE(Coop::DecodeDenyList(Json::array({-1})));
    REQUIRE_FALSE(Coop::DecodeDenyList(Json::array({"flag name"})));
    REQUIRE_FALSE(Coop::DecodeDenyList(Json::object()));
    Coop::State state{*denied};
    REQUIRE_FALSE(state.Merge({Kind::Bool, "a", 1, 101, 1, 0}));
    REQUIRE_FALSE(state.Merge({Kind::Quest, "a", 2, 102, 5, 999}));
    REQUIRE_FALSE(state.Merge({Kind::Bit, "a", 3, 101, 1, 0}));
    REQUIRE_FALSE(state.Merge({Kind::PouchAdd, "a", 4, 101, 1, 0}));
    REQUIRE(state.Merge({Kind::Int, "a", 5, 103, 10, 0}));
    REQUIRE(state.Merge({Kind::Int, "b", 1, 103, 4, 0}));
    REQUIRE(state.Find(Kind::Int, 103)->value == 4);
    REQUIRE(state.Merge({Kind::Enum, "a", 6, 103, 9, 0}));
    REQUIRE(state.Snapshot().size() == 2);
    REQUIRE(state.Snapshot()[0].origin.empty());
}

TEST_CASE("OoT co-op suppresses nested incoming sends and restores after exceptions",
          "[core][oot_coop]") {
    REQUIRE_FALSE(Coop::IncomingScope::SuppressSend());
    try {
        Coop::IncomingScope outer;
        REQUIRE(Coop::IncomingScope::SuppressSend());
        {
            Coop::IncomingScope inner;
            REQUIRE(Coop::IncomingScope::SuppressSend());
        }
        REQUIRE(Coop::IncomingScope::SuppressSend());
        throw 1;
    } catch (int) {
    }
    REQUIRE_FALSE(Coop::IncomingScope::SuppressSend());
}

TEST_CASE("OoT co-op requires endpoint title build and matching handshake", "[core][oot_coop]") {
    REQUIRE(Coop::Gate("127.0.0.1:43390", Trace::ProgramId, Trace::BuildId));
    REQUIRE(Coop::Gate("[::1]:43390", Trace::ProgramId, Trace::BuildId));
    REQUIRE_FALSE(Coop::Gate("", Trace::ProgramId, Trace::BuildId));
    REQUIRE_FALSE(Coop::Gate("0", Trace::ProgramId, Trace::BuildId));
    REQUIRE_FALSE(Coop::Gate("host:0", Trace::ProgramId, Trace::BuildId));
    REQUIRE_FALSE(Coop::Gate("host:65536", Trace::ProgramId, Trace::BuildId));
    REQUIRE_FALSE(Coop::Gate("host:43390", 1, Trace::BuildId));
    REQUIRE_FALSE(Coop::Gate("host:43390", Trace::ProgramId, "different"));
    Coop::Hello hello;
    hello.client_id = "a";
    auto packet = Coop::EncodeHello(hello);
    REQUIRE_FALSE(Coop::MatchesHello(packet, hello));
    packet["type"] = "hello_ack";
    REQUIRE(Coop::MatchesHello(packet, hello));
    packet["seed_hash"] = "1";
    REQUIRE_FALSE(Coop::MatchesHello(packet, hello));
    packet["seed_hash"] = "0";
    packet["build_id"] = "different";
    REQUIRE_FALSE(Coop::MatchesHello(packet, hello));
    packet["build_id"] = Trace::BuildId;
    packet["version"] = 2;
    REQUIRE_FALSE(Coop::MatchesHello(packet, hello));
}

TEST_CASE("OoT co-op has no enabled guest write path", "[core][oot_coop]") {
    REQUIRE_FALSE(Coop::GuestWritesVerified);
    for (const auto& site : Coop::Sites) {
        REQUIRE(site.offset == 0);
        const Change change{site.kind, "a", 1, 101, 1, 0};
        REQUIRE(Coop::GuestWriter{Coop::Strategy::GuestCall}.Apply(change) ==
                Coop::ApplyStatus::NotAppliedNoSite);
        REQUIRE(Coop::GuestWriter{Coop::Strategy::DirectWrite}.Apply(change) ==
                Coop::ApplyStatus::NotAppliedNoSite);
    }
    REQUIRE(Coop::GuestWriter{}.Apply({Kind::PouchTotal, {}, 0, 101, 5, 0}) ==
            Coop::ApplyStatus::NotAppliedNoSite);
}

TEST_CASE("OoT co-op apply decision respects observed local progress", "[core][oot_coop]") {
    Coop::State local;
    REQUIRE(local.Merge({Kind::Quest, {}, 0, 101, 8, 102}, false));
    REQUIRE_FALSE(Coop::ShouldApply({Kind::Quest, "b", 1, 101, 7, 999}, local));
    REQUIRE_FALSE(Coop::ShouldApply({Kind::Quest, "b", 2, 101, 8, 999}, local));
    REQUIRE(Coop::ShouldApply({Kind::Quest, "b", 3, 101, 9, 1}, local));
    REQUIRE(local.Merge({Kind::Bool, {}, 0, 103, 1, 0}, false));
    REQUIRE_FALSE(Coop::ShouldApply({Kind::Bool, "b", 4, 103, 1, 0}, local));
    REQUIRE_FALSE(Coop::ShouldApply({Kind::Bool, "b", 5, 104, 0, 0}, local));
    REQUIRE(Coop::ShouldApply({Kind::Bool, "b", 6, 104, 1, 0}, local));
    REQUIRE_FALSE(Coop::ShouldApply({Kind::PouchTotal, {}, 0, 105, 20, 0}, local));
}

TEST_CASE("OoT co-op installs snapshot highwaters after replay", "[core][oot_coop]") {
    Coop::State reconstructed;
    REQUIRE(reconstructed.Merge({Kind::PouchTotal, {}, 0, 101, 4, 0}, false));
    REQUIRE(reconstructed.Merge({Kind::PouchAdd, "a", 20, 101, 2, 0}));
    Coop::State remote;
    for (const auto& change : reconstructed.Snapshot()) {
        REQUIRE(remote.Merge(change, false));
    }
    REQUIRE(remote.AdoptSeen({{"a", 20}}));
    REQUIRE_FALSE(remote.Merge({Kind::PouchAdd, "a", 20, 101, 2, 0}));
    REQUIRE(remote.Merge({Kind::PouchAdd, "a", 21, 101, 1, 0}));
    REQUIRE(remote.Find(Kind::PouchTotal, 101)->value == 7);
    REQUIRE(remote.AdoptSeen({{"a", 1}}));
    REQUIRE_FALSE(remote.Merge({Kind::PouchAdd, "a", 21, 101, 1, 0}));
    REQUIRE_FALSE(remote.AdoptSeen({{"a", Coop::MaxSequence + 1}}));
}

TEST_CASE("OoT co-op only observes a resolved forward step request", "[core][oot_coop]") {
    struct Memory {
        std::map<u64, u64> words;
        bool IsValidVirtualAddressRange(u64 address, size_t) const {
            return words.contains(address);
        }
        u32 Read32(u64 address) const { return static_cast<u32>(words.at(address)); }
        u64 Read64(u64 address) const { return words.at(address); }
    } memory;
    constexpr u64 manager = 0x1000;
    constexpr u64 record = 0x2000;
    constexpr u64 steps = 0x3000;
    constexpr u64 state = record + 0x18;
    memory.words = {{manager + 0x40, 1}, {manager + 0x48, record}, {record + 8, 123},
                    {state + 0x64, 1}, {state + 0x68, 3}, {state + 0x70, steps},
                    {steps + 0x78, 999}, {steps + 0x90 + 0x78, 888},
                    {steps + 2 * 0x90 + 0x78, 777}};
    REQUIRE(Coop::StepIndex(memory, manager, 123, 777) == 2);
    REQUIRE_FALSE(Coop::StepIndex(memory, manager, 123, 888));
    REQUIRE_FALSE(Coop::StepIndex(memory, manager, 123, 999));
    REQUIRE_FALSE(Coop::StepIndex(memory, manager, 123, 666));
    REQUIRE_FALSE(Coop::StepIndex(memory, manager, 456, 777));
    memory.words[state + 0x68] = 257;
    REQUIRE_FALSE(Coop::StepIndex(memory, manager, 123, 777));
    memory.words.clear();
    REQUIRE_FALSE(Coop::StepIndex(memory, manager, 123, 777));
}
