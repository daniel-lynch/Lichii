// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <limits>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "core/arm/dynarmic/oot_coop_sync.h"
#include "core/arm/dynarmic/oot_coop_apply.h"

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
        const auto line = Coop::EncodeFrame(Coop::EncodeChange(change));
        REQUIRE(Coop::FrameLength(line.substr(0, 4)) == line.size() - 4);
        const auto packet = Coop::DecodeLine(line.substr(4));
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
    REQUIRE_FALSE(Coop::DecodeLine(R"({"value":true,"value":false})"));
    REQUIRE_FALSE(Coop::DecodeLine(std::string(Coop::MaxFrame + 1, ' ')));
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
    packet["version"] = 1;
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

TEST_CASE("OoT co-op v2 frames are bounded and policy matched", "[core][oot_coop]") {
    REQUIRE_FALSE(Coop::FrameLength(""));
    REQUIRE_FALSE(Coop::FrameLength(std::string(4, '\0')));
    REQUIRE_FALSE(Coop::FrameLength(std::string(4, '\xff')));
    REQUIRE(Coop::FrameLength(std::string{"\0\x10\0\0", 4}) == Coop::MaxFrame);
    REQUIRE_FALSE(Coop::FrameLength(std::string{"\0\x10\0\1", 4}));
    REQUIRE(Coop::EncodeFrame(Json{{"type", "heartbeat"}}) ==
            std::string("\0\0\0\x14", 4) + "{\"type\":\"heartbeat\"}");
    Coop::Hello hello;
    hello.client_id = "a";
    auto ack = Coop::EncodeHello(hello);
    ack["type"] = "hello_ack";
    REQUIRE(Coop::MatchesHello(ack, hello));
    ack["policy_id"] = "other";
    REQUIRE_FALSE(Coop::MatchesHello(ack, hello));
    REQUIRE(Coop::WireKind(Kind::Bool));
    REQUIRE(Coop::WireKind(Kind::Bit));
    REQUIRE_FALSE(Coop::WireKind(Kind::Quest));
    REQUIRE_FALSE(Coop::WireKind(Kind::PouchAdd));
}

namespace {
struct FakeGuest {
    struct Context {
        std::array<u64, 31> x{};
        std::array<std::array<u64, 2>, 32> v{};
        u64 pc{0x100}, sp{0x1000}, tls{0x2000};
        u32 pstate{0xf0000000}, fpcr{0x123}, fpsr{0x456};
        bool operator==(const Context&) const = default;
    } cpu;
    Coop::ApplyTarget target{true, false, 0, 0, 0x400, 0x800, 0x3000, {0x6000, 1, 101}};
    std::map<u64, u64> memory{{0x9000, 0xa0}};
    bool readable{true}, redirect_ok{true}, restore_ok{true}, recoverable{true}, return_stack{true};
    int writes{}, calls{}, restores{};
    using ContextType = Context;
    std::optional<Context> Capture() { return cpu; }
    std::optional<bool> ReadFlag(const Change&) {
        return readable ? std::optional<bool>{(memory.at(0x9000) & 1) != 0} : std::nullopt;
    }
    std::optional<Coop::ApplyTarget> Resolve(const Change&) { return target; }
    bool OrFlag(const Coop::ApplyTarget& t) {
        ++writes;
        memory.at(t.address) |= t.mask;
        return true;
    }
    bool Redirect(const Coop::ApplyTarget& t) {
        if (!redirect_ok) return false;
        ++calls;
        std::copy(t.arguments.begin(), t.arguments.end(), cpu.x.begin());
        cpu.x[30] = t.sentinel;
        cpu.pc = t.entry;
        cpu.sp = t.stack;
        return true;
    }
    bool AtReturnStack(const Coop::ApplyTarget&) { return return_stack; }
    bool CanRestore() { return recoverable; }
    bool Restore(const Context& saved) {
        if (!restore_ok) return false;
        ++restores;
        cpu = saved;
        return true;
    }
};
Coop::ApplyPolicy Allowed() { return {true, {Kind::Bool}, {{Kind::Bool, 101}}}; }
const Change Grant{Kind::Bool, "remote", 1, 101, 1, 0};
const Coop::DrainContext Safe{7, 9, 1, true};
} // namespace

TEST_CASE("OoT co-op application requires class key and proof gates", "[core][oot_coop]") {
    FakeGuest guest;
    Coop::GrantApplier<FakeGuest> off{{}};
    REQUIRE_FALSE(off.Queue(Grant));
    auto policy = Allowed();
    policy.classes.clear();
    Coop::GrantApplier<FakeGuest> no_class{policy};
    REQUIRE_FALSE(no_class.Queue(Grant));
    Coop::GrantApplier<FakeGuest> apply{Allowed()};
    REQUIRE_FALSE(apply.Queue({Kind::Bool, "remote", 2, 102, 1, 0}));
    REQUIRE_FALSE(apply.Queue({Kind::PouchAdd, "remote", 2, 101, 1, 0}));
    REQUIRE(apply.Queue(Grant));
    REQUIRE_FALSE(apply.Queue(Grant));
    REQUIRE(apply.Drain(guest, {7, 9, 1, false}) == Coop::ApplyEvent::Blocked);
    guest.target.verified = false;
    REQUIRE(apply.Drain(guest, Safe) == Coop::ApplyEvent::Blocked);
    REQUIRE(guest.calls == 0);
    REQUIRE(guest.writes == 0);
    REQUIRE(apply.Pending() == 1);
}

TEST_CASE("OoT co-op pure writes preserve unrelated save bits", "[core][oot_coop]") {
    FakeGuest guest;
    guest.target.pure_flag = true;
    guest.target.address = 0x9000;
    guest.target.mask = 1;
    Coop::GrantApplier<FakeGuest> apply{Allowed()};
    REQUIRE(apply.Queue(Grant));
    REQUIRE(apply.Drain(guest, Safe) == Coop::ApplyEvent::Committed);
    REQUIRE(guest.memory.at(0x9000) == 0xa1);
    REQUIRE(guest.writes == 1);
    REQUIRE(guest.calls == 0);
    REQUIRE(apply.Suppress(Grant));
    REQUIRE_FALSE(apply.Queue({Kind::Bool, "another", 99, 101, 1, 0}));
}

TEST_CASE("OoT co-op reads an existing baseline before granting", "[core][oot_coop]") {
    FakeGuest guest;
    guest.memory.at(0x9000) |= 1;
    Coop::GrantApplier<FakeGuest> apply{Allowed()};
    REQUIRE(apply.Queue(Grant));
    REQUIRE(apply.Drain(guest, Safe) == Coop::ApplyEvent::Committed);
    REQUIRE(guest.calls == 0);
    REQUIRE(guest.writes == 0);
    REQUIRE(apply.Pending() == 0);
}

TEST_CASE("OoT co-op sentinel restores context but waits for deferred storage", "[core][oot_coop]") {
    FakeGuest guest;
    guest.cpu.x[8] = 123;
    guest.cpu.v[31] = {456, 789};
    const auto saved = guest.cpu;
    Coop::GrantApplier<FakeGuest> apply{Allowed()};
    REQUIRE(apply.Queue(Grant));
    REQUIRE(apply.Drain(guest, Safe) == Coop::ApplyEvent::Started);
    REQUIRE(guest.cpu.x[0] == 0x6000);
    REQUIRE(guest.cpu.x[1] == 1);
    REQUIRE(guest.cpu.x[2] == 101);
    REQUIRE(guest.cpu.x[30] == 0x800);
    REQUIRE(guest.cpu.sp == 0x3000);
    REQUIRE(apply.Suppress(Grant));
    REQUIRE(apply.Drain(guest, Safe) == Coop::ApplyEvent::Blocked);
    REQUIRE(apply.Sentinel(guest, {8, 9, 2, true}, 0x800) == Coop::ApplyEvent::Blocked);
    REQUIRE(apply.Sentinel(guest, Safe, 0x804) == Coop::ApplyEvent::Blocked);
    guest.cpu = {};
    REQUIRE(apply.Sentinel(guest, Safe, 0x800) == Coop::ApplyEvent::Returned);
    REQUIRE(guest.cpu == saved);
    REQUIRE(apply.Phase() == Coop::ApplyPhase::AwaitingCommit);
    REQUIRE(apply.Drain(guest, {7, 9, 2, true}) == Coop::ApplyEvent::Blocked);
    REQUIRE(apply.Suppress(Grant));
    guest.memory.at(0x9000) |= 1;
    REQUIRE(apply.Drain(guest, {7, 9, 3, true}) == Coop::ApplyEvent::Committed);
    REQUIRE(apply.Suppress(Grant));
    REQUIRE(guest.calls == 1);
    REQUIRE(guest.restores == 1);
}

TEST_CASE("OoT co-op failed reads and redirects do not lose a pending grant", "[core][oot_coop]") {
    FakeGuest guest;
    const auto saved = guest.cpu;
    Coop::GrantApplier<FakeGuest> apply{Allowed()};
    REQUIRE(apply.Queue(Grant));
    guest.readable = false;
    REQUIRE(apply.Drain(guest, Safe) == Coop::ApplyEvent::Blocked);
    guest.readable = true;
    guest.redirect_ok = false;
    REQUIRE(apply.Drain(guest, Safe) == Coop::ApplyEvent::Blocked);
    REQUIRE(guest.cpu == saved);
    REQUIRE(apply.Pending() == 1);
    guest.redirect_ok = true;
    guest.target.stack = 0x3001;
    REQUIRE(apply.Drain(guest, Safe) == Coop::ApplyEvent::Blocked);
    guest.target.stack = 0x3000;
    REQUIRE(apply.Drain(guest, Safe) == Coop::ApplyEvent::Started);
}

TEST_CASE("OoT co-op watchdog restores only at a recoverable boundary", "[core][oot_coop]") {
    FakeGuest guest;
    const auto saved = guest.cpu;
    Coop::GrantApplier<FakeGuest> apply{Allowed()};
    REQUIRE(apply.Queue(Grant));
    REQUIRE(apply.Drain(guest, Safe) == Coop::ApplyEvent::Started);
    REQUIRE(apply.Watchdog(guest, {7, 9, 2, true}) == Coop::ApplyEvent::None);
    SECTION("safe recovery") {
        REQUIRE(apply.Watchdog(guest, {7, 9, 121, true}) == Coop::ApplyEvent::Recovered);
        REQUIRE(guest.cpu == saved);
        REQUIRE(guest.restores == 1);
    }
    SECTION("guest may hold a lock") {
        guest.recoverable = false;
        REQUIRE(apply.Watchdog(guest, {7, 9, 121, true}) == Coop::ApplyEvent::Quarantined);
        REQUIRE(guest.restores == 0);
    }
    REQUIRE(apply.Phase() == Coop::ApplyPhase::Quarantined);
    REQUIRE(apply.Suppress(Grant));
    REQUIRE(apply.Pending() == 1);
    REQUIRE(apply.Drain(guest, Safe) == Coop::ApplyEvent::Quarantined);
}

TEST_CASE("OoT co-op invalid sentinel and changed save quarantine the call", "[core][oot_coop]") {
    FakeGuest guest;
    Coop::GrantApplier<FakeGuest> apply{Allowed()};
    REQUIRE(apply.Queue(Grant));
    REQUIRE(apply.Drain(guest, Safe) == Coop::ApplyEvent::Started);
    SECTION("wrong return stack") { guest.return_stack = false; }
    SECTION("restore failure") { guest.restore_ok = false; }
    SECTION("generation changed") {
        REQUIRE(apply.Sentinel(guest, {7, 10, 2, true}, 0x800) == Coop::ApplyEvent::Quarantined);
    }
    if (apply.Phase() == Coop::ApplyPhase::Calling) {
        REQUIRE(apply.Sentinel(guest, Safe, 0x800) == Coop::ApplyEvent::Quarantined);
    }
    REQUIRE(apply.Suppress(Grant));
    REQUIRE(guest.restores == 0);
    apply.ResetAfterGuestStopped();
    REQUIRE_FALSE(apply.Suppress(Grant));
    REQUIRE(apply.Pending() == 0);
    REQUIRE(apply.Phase() == Coop::ApplyPhase::Idle);
}

TEST_CASE("OoT co-op a queued setter that never commits is not retried", "[core][oot_coop]") {
    FakeGuest guest;
    Coop::GrantApplier<FakeGuest> apply{Allowed()};
    REQUIRE(apply.Queue(Grant));
    REQUIRE(apply.Drain(guest, Safe) == Coop::ApplyEvent::Started);
    REQUIRE(apply.Sentinel(guest, Safe, 0x800) == Coop::ApplyEvent::Returned);
    REQUIRE(apply.Drain(guest, {7, 9, 121, true}) == Coop::ApplyEvent::Quarantined);
    REQUIRE(guest.calls == 1);
    REQUIRE(apply.Suppress(Grant));
}

TEST_CASE("OoT co-op grant classes default closed", "[core][oot_coop]") {
    REQUIRE(Coop::DecodeAllowClasses("")->empty());
    REQUIRE(Coop::DecodeAllowClasses("bool") == std::set{Kind::Bool});
    REQUIRE(Coop::DecodeAllowClasses("bool,bit") == std::set{Kind::Bool, Kind::Bit});
    REQUIRE_FALSE(Coop::DecodeAllowClasses("pouch"));
    REQUIRE_FALSE(Coop::DecodeAllowClasses("bool,"));
    REQUIRE_FALSE(Coop::DecodeAllowClasses("bool,quest"));
}

TEST_CASE("OoT co-op receipts cannot cross a loaded-save generation", "[core][oot_coop]") {
    FakeGuest guest;
    guest.memory.at(0x9000) |= 1;
    Coop::GrantApplier<FakeGuest> apply{Allowed()};
    REQUIRE(apply.Queue(Grant));
    REQUIRE(apply.Drain(guest, Safe) == Coop::ApplyEvent::Committed);
    REQUIRE(apply.Drain(guest, {7, 10, 3, true}) == Coop::ApplyEvent::Quarantined);
    apply.ResetAfterGuestStopped();
    REQUIRE(apply.Queue(Grant));
    REQUIRE(apply.Drain(guest, {7, 10, 3, true}) == Coop::ApplyEvent::Committed);
}

TEST_CASE("OoT co-op v2 grant fields cannot carry unchecked extensions", "[core][oot_coop]") {
    auto packet = Coop::EncodeChange(Grant);
    packet["extra"] = 1;
    REQUIRE_FALSE(Coop::DecodeChange(packet));
    packet.erase("extra");
    packet["rev"] = 1;
    REQUIRE(Coop::DecodeChange(packet));
    packet["rev"] = -1;
    REQUIRE_FALSE(Coop::DecodeChange(packet));
    REQUIRE_FALSE(Coop::DecodeChange(packet, true));
    packet = {{"type", "bool_set"}, {"hash", 101}, {"value", true}};
    REQUIRE(Coop::DecodeChange(packet, true));
}
