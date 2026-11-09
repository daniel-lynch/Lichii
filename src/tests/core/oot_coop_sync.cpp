// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <limits>
#include <string>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "core/arm/dynarmic/oot_coop_sync.h"
#include "core/arm/dynarmic/oot_coop_apply.h"
#include "core/arm/dynarmic/oot_coop_runtime.h"
#include "tests/core/oot_test_profile.h"

namespace {
namespace Coop = Core::OotCoop;
namespace Trace = Core::OotGameDataTrace;
using Coop::Change;
using Coop::Kind;
using Json = nlohmann::json;
namespace P = OotTestProfile;
constexpr u64 Wallet = P::Wallet;
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

TEST_CASE("OoT co-op requires endpoint title build and matching handshake", "[core][oot_coop]") {
    REQUIRE(Coop::Gate("1", Trace::ProgramId));
    REQUIRE(Coop::Gate("127.0.0.1:43390", Trace::ProgramId));
    REQUIRE(Coop::Gate("[::1]:43390", Trace::ProgramId));
    REQUIRE_FALSE(Coop::Gate("", Trace::ProgramId));
    REQUIRE_FALSE(Coop::Gate("0", Trace::ProgramId));
    REQUIRE_FALSE(Coop::Gate("host:0", Trace::ProgramId));
    REQUIRE_FALSE(Coop::Gate("host:65536", Trace::ProgramId));
    REQUIRE_FALSE(Coop::Gate("host:43390", 1));
    Coop::Hello hello;
    hello.build_id = P::BuildId;
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
    packet["build_id"] = P::BuildId;
    packet["version"] = 1;
    REQUIRE_FALSE(Coop::MatchesHello(packet, hello));
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

TEST_CASE("OoT co-op frame drain drives two peers without an echo", "[core][oot_coop]") {
    FakeGuest a, b;
    bool pure = false;
    SECTION("deferred setter") {}
    SECTION("proven pure flag") { pure = true; }
    b.target.pure_flag = pure;
    b.target.address = 0x9000;
    b.target.mask = 1;
    Coop::FrameDrain<FakeGuest> peer_a{Allowed()}, peer_b{Allowed()};
    std::vector<Change> from_a, from_b;
    std::vector<std::string> logs;
    const auto log = [&](std::string_view line) { logs.emplace_back(line); };
    const auto send_a = [&](Change c, bool) { from_a.push_back(c); };
    const auto send_b = [&](Change c, bool) { from_b.push_back(c); };
    peer_a.Frame(a, Safe, send_a, log);
    peer_b.Frame(b, Safe, send_b, log);
    REQUIRE(peer_a.BaselineReady());
    REQUIRE(peer_b.BaselineReady());
    REQUIRE(from_a.empty());
    REQUIRE(from_b.empty());
    a.memory.at(0x9000) |= 1; // The game commits a local check on A.
    peer_a.Frame(a, {7, 9, 2, true}, send_a, log);
    REQUIRE(from_a.size() == 1);
    auto wire = from_a.front();
    wire.origin = "A";
    wire.seq = 1;
    REQUIRE(peer_b.Queue(*Coop::DecodeChange(Coop::EncodeChange(wire))));
    REQUIRE(peer_b.Frame(b, {7, 9, 3, false}, send_b, log) == Coop::ApplyEvent::Blocked);
    REQUIRE(b.calls == 0);
    REQUIRE(b.writes == 0);
    peer_b.Frame(b, {7, 9, 4, true}, send_b, log);
    if (!pure) {
        REQUIRE(peer_b.Calling());
        Coop::ApplyEvent returned{};
        std::thread worker{[&] {
            returned = peer_b.Sentinel(b, {7, 9, 4, true}, b.target.sentinel, log);
        }};
        worker.join();
        REQUIRE(returned == Coop::ApplyEvent::Returned);
        peer_b.Frame(b, {7, 9, 5, true}, send_b, log);
        REQUIRE(from_b.empty());
        // Deferred queue consumer runs after the C++ callback and sentinel return.
        b.memory.at(0x9000) |= 1;
    }
    peer_b.Frame(b, {7, 9, 6, true}, send_b, log);
    peer_b.Frame(b, {7, 9, 7, true}, send_b, log);
    REQUIRE(from_b.empty());
    REQUIRE(b.calls == (pure ? 0 : 1));
    REQUIRE(b.writes == (pure ? 1 : 0));
    REQUIRE(peer_b.Suppress(wire));
    REQUIRE_FALSE(peer_b.Queue(wire));
    REQUIRE(std::count_if(logs.begin(), logs.end(), [](const auto& line) {
        return line.find("applied class=bool key=0000000000000065 value=1 path=") !=
               std::string::npos;
    }) == 1);
    REQUIRE(std::count_if(logs.begin(), logs.end(), [](const auto& line) {
        return line.find("suppressed class=bool") != std::string::npos &&
               line.find("echo=9-bool-0000000000000065") != std::string::npos;
    }) == 1);
}

TEST_CASE("OoT co-op frame observer quarantines rollback and incomplete baselines",
          "[core][oot_coop]") {
    FakeGuest guest;
    Coop::FrameDrain<FakeGuest> drain{Allowed()};
    std::vector<Change> sent;
    const auto send = [&](Change change, bool) { sent.push_back(change); };
    const auto log = [](std::string_view) {};
    guest.readable = false;
    REQUIRE(drain.Frame(guest, Safe, send, log) == Coop::ApplyEvent::Blocked);
    REQUIRE_FALSE(drain.BaselineReady());
    REQUIRE(sent.empty());
    guest.readable = true;
    guest.memory.at(0x9000) |= 1;
    drain.Frame(guest, Safe, send, log);
    REQUIRE(sent.size() == 1);
    REQUIRE(drain.Frame(guest, {8, 9, 2, true}, send, log) == Coop::ApplyEvent::Blocked);
    SECTION("rollback") { guest.memory.at(0x9000) &= ~u64{1}; }
    SECTION("unmapped save") { guest.readable = false; }
    REQUIRE(drain.Frame(guest, {7, 9, 3, true}, send, log) == Coop::ApplyEvent::Quarantined);
    REQUIRE_FALSE(drain.Queue(Grant));
    REQUIRE(guest.calls == 0);
}

TEST_CASE("OoT co-op typed key policies separate chest Bools and Bool64", "[core][oot_coop]") {
    constexpr u32 c1 = 0x5EED0001; // synthetic chest Bool
    const Json chest{{"bool", Json::array({c1})}};
    const auto keys = Coop::DecodeKeyPolicy(chest);
    REQUIRE(keys);
    REQUIRE(keys->contains({Kind::Bool, c1}));
    REQUIRE_FALSE(keys->contains({Kind::Bit, c1}));
    const auto legacy = Coop::DecodeKeyPolicy(Json::array({c1}));
    REQUIRE(legacy);
    REQUIRE(legacy->contains({Kind::Bit, c1}));
    REQUIRE(Coop::DecodeKeyPolicy(Json{{"bool", Json::array({c1})},
                                      {"bit", Json::array({0x1122334455667788ULL})}})->size() == 2);
    // Bool keys are 32-bit; the user-supplied file is the reviewed list (no compiled-in catalog).
    REQUIRE(Coop::DecodeKeyPolicy(Json{{"bool", Json::array({123})}}));
    REQUIRE(Coop::DecodeKeyPolicy(Json{{"bool", Json::array({c1})}, {"offsets", Json::object()}}));
    for (const auto& invalid : {Json{{"bool", Json::array({0x1122334455667788ULL})}},
                               Json{{"bool,bool", Json::array({c1})}},
                               Json{{"quest", Json::array({123})}},
                               Json{{"bit", Json::array({0})}}}) {
        REQUIRE_FALSE(Coop::DecodeKeyPolicy(invalid));
    }
    Coop::FrameDrain<FakeGuest> disabled{{true, {}, *keys}};
    REQUIRE_FALSE(disabled.Queue({Kind::Bool, "A", 1, c1, 1}));
}

TEST_CASE("OoT co-op rupee deltas are wallet-only events, never state", "[core][oot_coop]") {
    const Change five{Kind::Rupee, "B", 3, Wallet, 5};
    const auto packet = Coop::EncodeChange(five);
    REQUIRE(packet == Json{{"type", "rupee_add"}, {"origin", "B"}, {"seq", 3},
                           {"hash", P::Wallet}, {"delta", 5}});
    REQUIRE(Coop::DecodeChange(packet) == five);
    REQUIRE(Coop::WireKind(Kind::Rupee));
    auto with_rev = packet;
    with_rev["rev"] = 4;
    REQUIRE(Coop::DecodeChange(with_rev) == five);
    REQUIRE(Coop::DecodeChange({{"type", "rupee_add"}, {"origin", "B"}, {"seq", 3},
                                {"hash", P::Wallet}, {"delta", -999}})->value == -999);
    for (const auto& [field, value] : std::vector<std::pair<std::string, Json>>{
             {"delta", 0}, {"delta", 1000}, {"delta", -1000}, {"delta", true}, {"delta", 2.5},
             {"delta", "5"}, {"hash", 0}, {"value", true}, {"extra", 1}}) {
        auto bad = packet;
        bad[field] = value;
        REQUIRE_FALSE(Coop::DecodeChange(bad));
    }
    auto bare = packet;
    bare.erase("delta");
    REQUIRE_FALSE(Coop::DecodeChange(bare));
    // A delta is never snapshot state, with or without an origin.
    REQUIRE_FALSE(Coop::DecodeChange(packet, true));
    REQUIRE_FALSE(Coop::DecodeChange({{"type", "rupee_add"}, {"hash", P::Wallet}, {"delta", 5}}, true));
    REQUIRE(Coop::DecodeAllowClasses("bool,rupee") == std::set{Kind::Bool, Kind::Rupee});
    REQUIRE(Coop::DecodeAllowClasses("rupee") == std::set{Kind::Rupee});

    Coop::State state;
    REQUIRE(state.Merge(five));
    REQUIRE_FALSE(state.Merge(five)); // same origin/sequence: a duplicate, not a second +5
    REQUIRE(state.Merge({Kind::Rupee, "B", 4, Wallet, 5}));
    REQUIRE(state.Merge({Kind::Rupee, "C", 1, Wallet, -3}));
    REQUIRE_FALSE(state.Merge({Kind::Rupee, "C", 2, Wallet, 0}));
    REQUIRE_FALSE(state.Merge({Kind::Rupee, "C", 3, 0, 5}));
    REQUIRE(state.Snapshot().empty());
    REQUIRE_FALSE(state.Find(Kind::Rupee, Wallet));
    REQUIRE(Coop::ShouldApply(five, state));
}

TEST_CASE("OoT co-op rupee grants call once per delta and commit on return", "[core][oot_coop]") {
    FakeGuest guest;
    guest.target.arguments = {0x6000, 5, 7};
    const Change five{Kind::Rupee, "B", 3, Wallet, 5};
    Coop::GrantApplier<FakeGuest> closed{Allowed()};
    REQUIRE_FALSE(closed.Queue(five)); // class not allowed
    Coop::GrantApplier<FakeGuest> apply{{true, {Kind::Bool, Kind::Rupee}, {{Kind::Bool, 101}}, Wallet}};
    REQUIRE_FALSE(apply.Queue({Kind::Rupee, "B", 1, 101, 5, 0}));
    REQUIRE_FALSE(apply.Queue({Kind::Rupee, "B", 1, Wallet, 0, 0}));
    REQUIRE_FALSE(apply.Queue({Kind::Rupee, "B", 1, Wallet, 1000, 0}));
    REQUIRE(apply.Queue(five));
    REQUIRE(apply.Queue({Kind::Rupee, "B", 4, Wallet, 5, 0}));
    REQUIRE_FALSE(apply.Suppress(five));
    guest.readable = false; // a delta never reads a flag
    for (u64 tick = 1; tick <= 2; ++tick) {
        REQUIRE(apply.Drain(guest, {7, 9, tick * 3, true}) == Coop::ApplyEvent::Started);
        REQUIRE(apply.LastPath() == "call");
        REQUIRE(guest.cpu.x[1] == 5);
        REQUIRE(guest.cpu.x[2] == 7);
        REQUIRE(apply.Sentinel(guest, {7, 9, tick * 3 + 1, true}, 0x800) ==
                Coop::ApplyEvent::Returned);
        REQUIRE(apply.Drain(guest, {7, 9, tick * 3 + 2, true}) == Coop::ApplyEvent::Committed);
    }
    REQUIRE(guest.calls == 2);
    REQUIRE(guest.restores == 2);
    REQUIRE(apply.Pending() == 0);
    REQUIRE(apply.Drain(guest, {7, 9, 10, true}) == Coop::ApplyEvent::None);
    guest.target.pure_flag = true; // a delta never takes the data-or path
    REQUIRE(apply.Queue({Kind::Rupee, "B", 5, Wallet, 5, 0}));
    REQUIRE(apply.Drain(guest, {7, 9, 11, true}) == Coop::ApplyEvent::Blocked);
    REQUIRE(guest.writes == 0);
    REQUIRE(apply.Pending() == 1);
}

TEST_CASE("OoT co-op loads game offsets only from the private profile", "[core][oot_coop]") {
    using Trace::Point;
    // Synthetic values: the real ones live only in the user-supplied file.
    Json build{{"manager_cell", "0x200000"},
               {"pouch_cell", 0x200100},
               {"wallet_hash", "0x0BADCAFE"},
               {"sites", {{"frame", Json::array({"0x103000"})}, {"bit_read", Json::array({"0x104004", 0x106004})}}},
               {"entries", {{"bool_setter", {{"at", "0x110000"}, {"words", Json::array({"0xD503237F", 1})}}},
                            {"rupee_entry", {{"at", 0x112000}, {"words", Json::array({2})}}}}}};
    const Json document{{"bool", Json::array({101})}, {"offsets", {{P::BuildId, build}}}};
    const auto profile = Coop::DecodeProfile(document, P::BuildId);
    REQUIRE(profile);
    REQUIRE(profile->build_id == P::BuildId);
    REQUIRE(profile->manager_cell == 0x200000);
    REQUIRE(profile->pouch_cell == 0x200100);
    REQUIRE(profile->wallet_hash == 0x0BADCAFE);
    REQUIRE(profile->sites.size() == 3);
    REQUIRE(profile->SiteOffset(Point::Frame) == 0x103000);
    for (const auto& site : profile->sites)
        REQUIRE(site.instruction == Trace::ReplacedInstruction(site.point));
    REQUIRE(profile->bool_setter.offset == 0x110000);
    REQUIRE(profile->bool_setter.words == std::vector<u32>{0xD503237F, 1});
    REQUIRE(profile->rupee_entry.words == std::vector<u32>{2});
    REQUIRE_FALSE(profile->bit_setter);
    // The key half of the same file still decodes.
    REQUIRE(Coop::DecodeKeyPolicy(document)->contains({Kind::Bool, 101}));
    // Another build, or no offsets at all: nothing.
    REQUIRE_FALSE(Coop::DecodeProfile(document, "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"));
    REQUIRE_FALSE(Coop::DecodeProfile(Json{{"bool", Json::array({101})}}, P::BuildId));
    REQUIRE_FALSE(Coop::DecodeProfile(document, "short"));
    const auto broken = [&](const char* field, Json value) {
        auto copy = build;
        copy[field] = std::move(value);
        return Coop::DecodeProfile(Json{{"offsets", {{P::BuildId, copy}}}}, P::BuildId);
    };
    REQUIRE_FALSE(broken("unknown", 1));
    REQUIRE_FALSE(broken("manager_cell", "0x200004"));   // a pointer cell is 8-aligned
    REQUIRE_FALSE(broken("manager_cell", 0));
    REQUIRE_FALSE(broken("manager_cell", "200000"));      // strings must be 0x hex
    REQUIRE_FALSE(broken("wallet_hash", "0x100000000"));
    REQUIRE_FALSE(broken("sites", Json{{"frame", Json::array({"0x103002"})}}));               // unaligned
    REQUIRE_FALSE(broken("sites", Json{{"nowhere", Json::array({"0x103000"})}}));
    REQUIRE_FALSE(broken("sites", Json{{"frame", Json::array({"0x103000"})}, {"step_set", Json::array({"0x103000"})}}));
    REQUIRE_FALSE(broken("entries", Json{{"bool_setter", {{"at", "0x110000"}, {"words", Json::array()}}}}));
    REQUIRE_FALSE(broken("entries", Json{{"mystery", {{"at", "0x110000"}, {"words", Json::array({1})}}}}));
}

TEST_CASE("OoT co-op item gives are named events, never state", "[core][oot_coop]") {
    Change give{Kind::Item, "B", 3, Coop::PackItem({6, 7, {0, 0, 1, 0}}), 20};
    give.name = "Item_Test_Bomb";
    const auto packet = Coop::EncodeChange(give);
    REQUIRE(packet == Json{{"type", "item_give"}, {"origin", "B"}, {"seq", 3}, {"name", "Item_Test_Bomb"},
                           {"category", 6}, {"count", 20}, {"get", 7},
                           {"flags", Json::array({0, 0, 1, 0})}});
    REQUIRE(Coop::DecodeChange(packet) == give);
    auto with_rev = packet;
    with_rev["rev"] = 9;
    REQUIRE(Coop::DecodeChange(with_rev) == give);
    for (const auto& [field, value] : std::vector<std::pair<std::string, Json>>{
             {"name", ""}, {"name", "has space"}, {"name", std::string(64, 'a')}, {"name", 5},
             {"count", 0}, {"count", 1000}, {"count", -1}, {"category", 16}, {"get", 16},
             {"flags", Json::array({0, 0, 0})}, {"flags", Json::array({0, 0, 0, 256})},
             {"hash", 1}, {"extra", 1}}) {
        auto bad = packet;
        bad[field] = value;
        CAPTURE(field);
        REQUIRE_FALSE(Coop::DecodeChange(bad));
    }
    REQUIRE_FALSE(Coop::DecodeChange(packet, true)); // never snapshot state
    REQUIRE(Coop::DecodeAllowClasses("bool,item") == std::set{Kind::Bool, Kind::Item});
    Coop::State state;
    REQUIRE(state.Merge(give));
    REQUIRE_FALSE(state.Merge(give)); // the same origin/sequence again: a duplicate, no second give
    auto next = give;
    next.seq = 4;
    REQUIRE(state.Merge(next)); // a new give of the same item adds again
    REQUIRE(state.Snapshot().empty());
    REQUIRE(Coop::ShouldApply(give, state));
    Coop::GrantApplier<FakeGuest> closed{Allowed()};
    REQUIRE_FALSE(closed.Queue(give)); // the item class is off by default
    Coop::GrantApplier<FakeGuest> open{{true, {Kind::Item}, {}}};
    REQUIRE(open.Queue(give));
    REQUIRE(open.Queue(next));
    REQUIRE_FALSE(open.Suppress(give));
    auto bad = give;
    bad.value = 0;
    REQUIRE_FALSE(open.Queue(bad));
}

TEST_CASE("OoT co-op small-key gains are dungeon events, never state", "[core][oot_coop]") {
    const Change gain{Kind::Key, "B", 3, 0x1234, 1};
    const auto packet = Coop::EncodeChange(gain);
    REQUIRE(packet == Json{{"type", "key_add"}, {"origin", "B"}, {"seq", 3}, {"hash", 0x1234}, {"delta", 1}});
    REQUIRE(Coop::DecodeChange(packet) == gain);
    REQUIRE(Coop::WireKind(Kind::Key));
    REQUIRE(Coop::EventKind(Kind::Key));
    for (const auto& [field, value] : std::vector<std::pair<std::string, Json>>{
             {"delta", 0}, {"delta", -1}, {"delta", 33}, {"delta", true}, {"hash", 0},
             {"hash", 0x100000000ULL}, {"extra", 1}}) {
        auto bad = packet;
        bad[field] = value;
        REQUIRE_FALSE(Coop::DecodeChange(bad));
    }
    REQUIRE_FALSE(Coop::DecodeChange(packet, true)); // never snapshot state
    REQUIRE(Coop::DecodeAllowClasses("bool,dungeon") == std::set{Kind::Bool, Kind::Key});
    Coop::State state;
    REQUIRE(state.Merge(gain));
    REQUIRE_FALSE(state.Merge(gain)); // a duplicate delivery is not a second key
    REQUIRE(state.Merge({Kind::Key, "C", 1, 0x1234, 1})); // two players, two keys
    REQUIRE(state.Snapshot().empty());
    REQUIRE(Coop::ShouldApply(gain, state));
    Coop::ApplyPolicy policy{true, {Kind::Key}, {{Kind::Key, 0x1234}}};
    REQUIRE(policy.Allows(gain));
    REQUIRE_FALSE(policy.Allows({Kind::Key, "B", 4, 0x9999, 1}));
    REQUIRE_FALSE(Coop::ApplyPolicy{true, {Kind::Bool}, {{Kind::Key, 0x1234}}}.Allows(gain));
}

TEST_CASE("OoT co-op key watch forwards gains, never spends or our own credit", "[core][oot_coop]") {
    Coop::KeyWatch watch;
    REQUIRE(watch.Step(2, 1) == 0);      // baseline
    REQUIRE(watch.Step(3, 2) == 1);      // a key picked up: total and count rise
    REQUIRE(watch.Step(3, 1) == 0);      // a key spent on a door: per-player, nothing sent
    REQUIRE(watch.Step(4, 1) == 1);      // a gain seen only in the total (count unchanged frame)
    REQUIRE(watch.Step(4, 2) == 1);      // a gain seen only in the count (total not maintained)
    watch.credit = 2;                    // two received gains applied here
    REQUIRE(watch.Step(5, 3) == 0);      // the first one shows up: explained by the credit
    REQUIRE(watch.Step(7, 5) == 1);      // the second plus a real local pickup in the same frame
    REQUIRE(watch.credit == 0);
    REQUIRE(watch.Step(std::nullopt, 5) == 0); // unreadable: reset, no forward
    REQUIRE_FALSE(watch.total);
    REQUIRE(watch.Step(9, 6) == 0);      // a fresh baseline after the reset
}

TEST_CASE("OoT co-op key file extras: per-player deny and the dungeon class", "[core][oot_coop]") {
    const Json file{{"bool", {1, 2, 3}},
                    {"deny", {{"bool", {2, 7}}}},
                    {"dungeon", {{"bool", {11, 12}}, {"keys", {{21, 22}, {31, 32}}}}},
                    {"offsets", Json::object()}};
    const auto keys = Coop::DecodeKeyPolicy(file);
    REQUIRE(keys);
    REQUIRE(keys->size() == 3); // the extras are not allow-list entries by themselves
    const auto extras = Coop::DecodeKeyExtras(file);
    REQUIRE(extras);
    REQUIRE(extras->deny == std::set<u64>{2, 7});
    REQUIRE(extras->dungeon_bools == std::vector<u32>{11, 12});
    REQUIRE(extras->dungeon_keys.size() == 2);
    REQUIRE(extras->dungeon_keys[1].total == 31);
    REQUIRE(extras->dungeon_keys[1].count == 32);
    REQUIRE(Coop::DecodeKeyExtras(Json::array({1, 2}))->deny.empty()); // legacy bare array
    for (const Json& bad : {Json{{"deny", {{"bit", {1}}}}}, Json{{"deny", {{"bool", {0}}}}},
                            Json{{"deny", {{"bool", {0x100000000ULL}}}}},
                            Json{{"dungeon", {{"keys", {{21, 21}}}}}}, Json{{"dungeon", {{"keys", {{21}}}}}},
                            Json{{"dungeon", {{"other", Json::array()}}}}, Json{{"deny", 1}}}) {
        REQUIRE_FALSE(Coop::DecodeKeyExtras(bad));
    }
    // The per-player Bools are dropped by protocol state too (the session's deny set).
    Coop::State state{extras->deny};
    REQUIRE_FALSE(state.Merge({Kind::Bool, "B", 1, 7, 1}));
    REQUIRE(state.Merge({Kind::Bool, "B", 2, 1, 1}));
}

TEST_CASE("OoT co-op profile: wallet observation and the Int add entry", "[core][oot_coop]") {
    const auto load = [](const Json& fields) {
        Json offsets = fields;
        offsets["manager_cell"] = "0x200000";
        return Coop::DecodeProfile(Json{{"offsets", {{P::BuildId, offsets}}}}, P::BuildId);
    };
    const auto loaded = load({{"rupee_callers", {"0x160004"}},
                              {"sites", {{"rupee_add", {"0x112000"}}}},
                              {"entries", {{"int_add", {{"at", "0x114000"}, {"words", {"0xD503237F"}}}}}}});
    REQUIRE(loaded);
    REQUIRE(loaded->rupee_callers == std::vector<u64>{0x160004});
    REQUIRE(loaded->int_add.offset == 0x114000);
    REQUIRE(loaded->SiteOffset(Trace::Point::RupeeAdd) == 0x112000);
    REQUIRE(loaded->sites.back().instruction == Trace::MovW8Pending);
    REQUIRE_FALSE(load({{"rupee_callers", {"0x160005"}}}));
    REQUIRE_FALSE(load({{"rupee_callers", "0x160004"}}));
}

TEST_CASE("OoT co-op same-chest race: one opener's gives per chest, every order", "[core][oot_coop]") {
    using Open = Coop::ChestClaims::Open;
    constexpr u32 K = 0xC1;
    const auto give = [](std::string origin, u64 seq, u32 key) {
        Change c{Kind::Item, std::move(origin), seq, Coop::PackItem({6, 7, {0, 0, 1, 0}}), 1};
        c.name = "Item_Test_Slingshot";
        c.origin_key = key;
        return c;
    };
    SECTION("local first: our open owns the chest, the peer's later gives are dropped") {
        Coop::ChestClaims claims;
        REQUIRE(claims.LocalOpen(K, 0x100) == Open::Ours);
        REQUIRE_FALSE(claims.SkipLocal(0x100));
        claims.Hold(0x100);
        REQUIRE(claims.End(0x100) == K); // our held give goes out tagged with the chest
        REQUIRE_FALSE(claims.AcceptRemote(give("R", 1, K)));
        REQUIRE(claims.Owner(K) == "us");
    }
    SECTION("remote first: the peer's claim arrived, our open skips our gives, the peer's apply") {
        Coop::ChestClaims claims;
        claims.RemoteClaim(K, "R"); // its bool_set
        REQUIRE(claims.LocalOpen(K, 0x200) == Open::SkipLocal);
        REQUIRE(claims.SkipLocal(0x200));
        REQUIRE(claims.AcceptRemote(give("R", 2, K)));
        REQUIRE_FALSE(claims.AcceptRemote(give("S", 1, K))); // a third opener's copy
        REQUIRE(claims.End(0x200) == K);
        REQUIRE_FALSE(claims.SkipLocal(0x200));
    }
    SECTION("simultaneous: we gave before the chest flag, the peer's give is queued: we take over") {
        Coop::ChestClaims claims;
        claims.Hold(0x300);                             // our give ran first in the event
        REQUIRE(claims.AcceptRemote(give("R", 1, K)));  // the peer's give arrives mid-event (queued)
        REQUIRE(claims.LocalOpen(K, 0x300) == Open::TakeOver); // the session drops its queued gives
        REQUIRE_FALSE(claims.AcceptRemote(give("R", 2, K)));   // and every later one
        REQUIRE(claims.Owner(K) == "us");
    }
    SECTION("simultaneous: both opens before either claim arrives: each keeps its own") {
        Coop::ChestClaims a, b;
        REQUIRE(a.LocalOpen(K, 1) == Open::Ours);
        REQUIRE(b.LocalOpen(K, 1) == Open::Ours);
        b.RemoteClaim(K, "A"); // A's bool_set reaches B: B already owns it
        REQUIRE_FALSE(a.AcceptRemote(give("B", 1, K)));
        REQUIRE_FALSE(b.AcceptRemote(give("A", 1, K)));
    }
    SECTION("untagged gives always apply; an event with no chest or two chests sends untagged") {
        Coop::ChestClaims claims;
        REQUIRE(claims.AcceptRemote(give("R", 1, 0)));
        claims.Hold(0x400);
        REQUIRE(claims.End(0x400) == 0);
        claims.LocalOpen(K, 0x500);
        claims.LocalOpen(K + 1, 0x500);
        REQUIRE(claims.End(0x500) == 0);
    }
}

TEST_CASE("OoT co-op item give origin key: wire and queued-give removal", "[core][oot_coop]") {
    Change give{Kind::Item, "A", 4, Coop::PackItem({6, 7, {0, 0, 1, 0}}), 1};
    give.name = "Item_Test_Slingshot";
    give.origin_key = 0xC1;
    const auto packet = Coop::EncodeChange(give);
    REQUIRE(packet.at("origin_key") == 0xC1);
    REQUIRE(Coop::DecodeChange(packet) == give);
    give.origin_key = 0;
    REQUIRE_FALSE(Coop::EncodeChange(give).contains("origin_key"));
    for (const Json& bad : {Json(0), Json(-1), Json(0x100000000ULL), Json("c1")}) {
        auto p = packet;
        p["origin_key"] = bad;
        REQUIRE_FALSE(Coop::DecodeChange(p));
    }
    // DropPending removes queued gives of a chest, never another chest's or another class.
    struct NoBackend { using Context = int; };
    Coop::GrantApplier<NoBackend> applier{{true, {Kind::Item, Kind::Rupee}, {}, Wallet}};
    Change mine = give, other = give;
    mine.origin_key = 0xC1;
    other.origin_key = 0xC2;
    other.seq = 5;
    REQUIRE(applier.Queue(mine));
    REQUIRE(applier.Queue(other));
    REQUIRE(applier.Queue({Kind::Rupee, "A", 6, Wallet, 5}));
    REQUIRE(applier.DropPending([](const Change& c) { return c.kind == Kind::Item && c.origin_key == 0xC1; }) == 1);
    REQUIRE(applier.Pending() == 2);
    // Profile: the chest members map.
    const auto load = [](const Json& members) {
        return Coop::DecodeProfile(Json{{"offsets", {{P::BuildId, {{"manager_cell", "0x200000"},
                                                                  {"chest_members", members}}}}}}, P::BuildId);
    };
    const auto loaded = load(Json::array({Json::array({"0x11", "0xC1"}), Json::array({0x12, 0xC2})}));
    REQUIRE(loaded);
    REQUIRE(loaded->chest_members.at(0x11) == 0xC1);
    REQUIRE(loaded->chest_members.at(0x12) == 0xC2);
    REQUIRE_FALSE(load(Json::array({Json::array({"0x11", "0xC1"}), Json::array({"0x11", "0xC3"})})));
    REQUIRE_FALSE(load(Json::array({Json::array({0, 1})})));
}

TEST_CASE("OoT co-op animation hello requires the exact table hash", "[core][oot_coop]") {
    Coop::Hello hello;
    hello.client_id = "synthetic";
    hello.animation_hash = std::string(64, 'a');
    auto packet = Coop::EncodeHello(hello);
    REQUIRE(packet.at("version") == 4);
    REQUIRE(packet.at("animation").at("av") == 1);
    packet["type"] = "hello_ack";
    REQUIRE(Coop::MatchesHello(packet, hello));
    packet["animation"]["av"] = 1.0;
    REQUIRE_FALSE(Coop::MatchesHello(packet, hello));
    packet["animation"]["av"] = 1;
    packet["animation"]["table_sha256"] = std::string(64, 'b');
    REQUIRE_FALSE(Coop::MatchesHello(packet, hello));
    packet.erase("animation");
    REQUIRE_FALSE(Coop::MatchesHello(packet, hello));
}
