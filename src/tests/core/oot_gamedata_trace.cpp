// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/nx2_event_trace.h"
#include "core/arm/dynarmic/oot_gamedata_trace.h"
#include "core/arm/dynarmic/oot_coop_runtime.h"
#include "dynarmic/interface/A64/a64.h"

namespace {

namespace Trace = Core::OotGameDataTrace;
using Trace::Point;
using Common::Nx2EventTrace::Murmur3;

constexpr u64 Base = 0x10000000;
constexpr u64 Stop = 0x08000000;
constexpr u64 Data = 0x40000000;

constexpr u32 Ret = 0xD65F03C0;
constexpr u32 Svc0 = 0xD4000001;
constexpr u32 MovzX9Seven = 0xD28000E9; // movz x9, #7

/// Guest memory as bytes (what the handler reads) and the code words the JIT fetches.
class TraceEnvironment final : public Dynarmic::A64::UserCallbacks {
public:
    bool enabled{};
    Dynarmic::A64::Jit* jit{};
    std::optional<Trace::Session> session;
    std::map<u64, u32> code;
    std::map<u64, u8> bytes;
    std::vector<std::string> lines;
    u32 breakpoints{};
    std::function<void(u64)> on_breakpoint;

    void Code(u64 address, u32 word) {
        code[address] = word;
        Write32(address, word);
    }
    void Write32(u64 address, u32 value) {
        for (u32 i = 0; i < 4; ++i) {
            bytes[address + i] = static_cast<u8>(value >> (8 * i));
        }
    }
    void Write64(u64 address, u64 value) {
        Write32(address, static_cast<u32>(value));
        Write32(address + 4, static_cast<u32>(value >> 32));
    }

    // The memory interface Trace::Handle reads through (Core::Memory::Memory's names).
    bool IsValidVirtualAddressRange(u64 address, u64 size) const {
        for (u64 i = 0; i < size; ++i) {
            if (!bytes.contains(address + i)) {
                return false;
            }
        }
        return true;
    }
    u8 Read8(u64 address) {
        return bytes.at(address);
    }
    u32 Read32(u64 address) {
        u32 value = 0;
        for (u32 i = 0; i < 4; ++i) {
            value |= u32{bytes.at(address + i)} << (8 * i);
        }
        return value;
    }
    u64 Read64(u64 address) {
        return u64{Read32(address)} | (u64{Read32(address + 4)} << 32);
    }

    std::optional<u32> MemoryReadCode(u64 address) override {
        const auto it = code.find(address);
        if (it == code.end()) {
            return std::nullopt;
        }
        return enabled ? Trace::InstructionFor(Base, address, it->second) : it->second;
    }
    u64 MemoryRead(u64, size_t) override {
        FAIL("Unexpected memory read");
        return 0;
    }
    Dynarmic::A64::Vector MemoryRead128(u64) override {
        FAIL("Unexpected memory read");
        return {};
    }
    void MemoryWrite(u64, u64, size_t) override {
        FAIL("Unexpected memory write");
    }
    void MemoryWrite128(u64, Dynarmic::A64::Vector) override {
        FAIL("Unexpected memory write");
    }
    void CallSVC(u32) override {
        jit->HaltExecution();
    }
    void ExceptionRaised(u64 pc, Dynarmic::A64::Exception exception) override {
        REQUIRE(exception == Dynarmic::A64::Exception::Breakpoint);
        if (on_breakpoint) { on_breakpoint(pc); return; }
        REQUIRE(session.has_value());
        ++breakpoints;
        std::string line;
        REQUIRE(Trace::Handle(*session, pc, *jit, *this, &line));
        lines.push_back(std::move(line));
    }
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override {
        return 1000;
    }
    u64 GetCNTPCT() override {
        return 0;
    }
};

Dynarmic::A64::UserConfig Config(TraceEnvironment& env) {
    Dynarmic::A64::UserConfig config;
    config.callbacks = &env;
    config.enable_cycle_counting = false;
    return config;
}

u64 Site(Point point) {
    for (const auto& site : Trace::Sites) {
        if (site.point == point) {
            return Base + site.offset;
        }
    }
    FAIL("No such site");
    return 0;
}

/// Runs from `entry` with x0..x5 = `args`, x8 = all ones, every other register a marker, and
/// checks the JIT ends in the same state the untraced code reaches.
void RunAndCheck(TraceEnvironment& env, u64 entry, const std::array<u64, 6>& args,
                 u64 expected_x8, u64 expected_x9) {
    Dynarmic::A64::Jit jit{Config(env)};
    env.jit = &jit;
    jit.SetPC(entry);
    for (size_t i = 0; i < 30; ++i) {
        jit.SetRegister(i, 0x12340000 + i);
    }
    for (size_t i = 0; i < args.size(); ++i) {
        jit.SetRegister(i, args[i]);
    }
    jit.SetRegister(8, ~0ULL);
    jit.SetRegister(30, Stop);
    jit.SetSP(0x20000000);
    jit.SetPstate(0xA0000000);
    jit.Run();
    REQUIRE(jit.GetPC() == Stop + 4);
    for (size_t i = 0; i < args.size(); ++i) {
        REQUIRE(jit.GetRegister(i) == args[i]);
    }
    REQUIRE(jit.GetRegister(8) == expected_x8);
    REQUIRE(jit.GetRegister(9) == expected_x9);
    for (size_t i = 10; i < 30; ++i) {
        REQUIRE(jit.GetRegister(i) == 0x12340000 + i);
    }
    REQUIRE(jit.GetRegister(30) == Stop);
    REQUIRE(jit.GetSP() == 0x20000000);
    REQUIRE(jit.GetPstate() == 0xA0000000);
}

} // namespace

TEST_CASE("GameData trace sites need the traced build's base and original instructions",
          "[oot_gamedata_trace]") {
    for (const auto& site : Trace::Sites) {
        REQUIRE(Trace::Identify(Base, Base + site.offset, site.instruction) == site.point);
        REQUIRE(Trace::InstructionFor(Base, Base + site.offset, site.instruction) ==
                Trace::Breakpoint);
        // No module registered (another build), or a different word there (a mod, a cheat, a
        // debugger breakpoint): the JIT gets the word unchanged.
        REQUIRE(Trace::Identify(0, site.offset, site.instruction) == Point::None);
        REQUIRE(Trace::InstructionFor(0, site.offset, site.instruction) == site.instruction);
        REQUIRE(Trace::Identify(Base, Base + site.offset, Trace::Breakpoint) == Point::None);
        REQUIRE(Trace::InstructionFor(Base, Base + site.offset, 0xD503201F) == 0xD503201F);
        REQUIRE(Trace::Identify(Base, Base + site.offset + 4, site.instruction) == Point::None);
    }
    REQUIRE(Trace::Identify(Base, Base - 4, Trace::Pacibsp) == Point::None);
    REQUIRE(Trace::BuildId.size() == 40);
    REQUIRE(Trace::BuildId.starts_with("E0F3CCFAF50065AE"));
}

TEST_CASE("GameData trace: the step setter hook keeps its mov w8, #1",
          "[oot_gamedata_trace]") {
    const u64 entry = Site(Point::StepSet) - 4;
    const u64 manager = Data;
    const u64 records = Data + 0x1000;
    const u32 challenge = Murmur3("TraceTest_Challenge");
    for (const bool enabled : {false, true}) {
        TraceEnvironment env;
        env.enabled = enabled;
        env.session.emplace(nullptr, Base, 1);
        env.Code(entry, MovzX9Seven);
        env.Code(entry + 4, Trace::MovW8One);
        env.Code(entry + 8, Ret);
        env.Code(Stop, Svc0);
        // One challenge record in the manager's first table, currently at Step06.
        env.Write32(manager + 0x40, 1);
        env.Write64(manager + 0x48, records);
        env.Write32(manager + 0x50, 0);
        env.Write64(manager + 0x58, 0);
        env.Write32(manager + 0x60, 0);
        env.Write64(manager + 0x68, 0);
        env.Write32(records + 8, challenge);
        env.Write32(records + 0x10, Murmur3("Step06"));

        RunAndCheck(env, entry, {manager, challenge, Murmur3("Step07"), 1, 1, 0}, 1, 7);
        if (enabled) {
            REQUIRE(env.breakpoints == 1);
            REQUIRE(env.lines.size() == 1);
            const std::string& line = env.lines[0];
            INFO(line);
            REQUIRE(line.starts_with("gamedata step-set "));
            REQUIRE(line.find("step=Step07 (0xee867c27)") != std::string::npos);
            REQUIRE(line.find("current=Step06 (0x21d6fa38)") != std::string::npos);
            REQUIRE(line.find(fmt::format("challenge={:#010x}", challenge)) != std::string::npos);
            REQUIRE(line.find("ui=1 arg4=1 force=0") != std::string::npos);
        } else {
            REQUIRE(env.breakpoints == 0);
        }
    }
}

TEST_CASE("GameData trace: the step apply hook reports the direction",
          "[oot_gamedata_trace]") {
    const u64 entry = Site(Point::StepApply);
    const u64 record = Data;
    const u64 steps = Data + 0x1000;
    for (const bool enabled : {false, true}) {
        TraceEnvironment env;
        env.enabled = enabled;
        env.session.emplace(nullptr, Base, 1);
        env.Code(entry, Trace::Pacibsp);
        env.Code(entry + 4, MovzX9Seven);
        env.Code(entry + 8, Ret);
        env.Code(Stop, Svc0);
        // The record: currently Step03 (index 2 of 3), pending Step01.
        const u64 x0 = record + 0x18;
        env.Write32(x0 - 0x10, Murmur3("TraceTest_Challenge"));
        env.Write32(x0 - 0x8, Murmur3("Step03"));
        env.Write32(x0 + 0x64, 2);
        env.Write32(x0 + 0x68, 3);
        env.Write64(x0 + 0x70, steps);
        env.Write32(x0 + 0x8c, Murmur3("Step01"));
        for (u32 i = 0; i < 3; ++i) {
            env.Write32(steps + i * 0x90 + 0x78, Murmur3(fmt::format("Step{:02}", i + 1)));
        }

        RunAndCheck(env, entry, {x0, 1, 0x12340002, 0x12340003, 0x12340004, 0x12340005}, ~0ULL,
                    7);
        if (enabled) {
            REQUIRE(env.lines.size() == 1);
            const std::string& line = env.lines[0];
            INFO(line);
            REQUIRE(line.starts_with("gamedata step-apply "));
            REQUIRE(line.find("from=Step03 (") != std::string::npos);
            REQUIRE(line.find("#2 to=Step01 (") != std::string::npos);
            REQUIRE(line.find("#0 of 3 backwards") != std::string::npos);
        } else {
            REQUIRE(env.breakpoints == 0);
        }
    }
}

TEST_CASE("GameData trace: the HavePlayedEvent hook reads the event name",
          "[oot_gamedata_trace]") {
    const u64 entry = Site(Point::PlayedEvent);
    const u64 task = Data;
    const u64 object = Data + 0x100;
    const u64 text = Data + 0x200;
    const std::string name = "TraceTest_Event";
    for (const bool enabled : {false, true}) {
        TraceEnvironment env;
        env.enabled = enabled;
        env.session.emplace(nullptr, Base, 1);
        env.Code(entry, Trace::Pacibsp);
        env.Code(entry + 4, MovzX9Seven);
        env.Code(entry + 8, Ret);
        env.Code(Stop, Svc0);
        env.Write64(task + 0x10, object);
        env.Write64(object + 8, text);
        // The length counts the terminator, which the game writes before hashing.
        env.Write32(object + 0x10, static_cast<u32>(name.size() + 1));
        for (size_t i = 0; i < name.size(); ++i) {
            env.bytes[text + i] = static_cast<u8>(name[i]);
        }
        env.bytes[text + name.size()] = 0xAA; // not yet terminated

        RunAndCheck(env, entry, {task, 0x12340001, 0x12340002, 0x12340003, 0x12340004,
                                 0x12340005},
                    ~0ULL, 7);
        if (enabled) {
            REQUIRE(env.lines.size() == 1);
            INFO(env.lines[0]);
            REQUIRE(env.lines[0].starts_with(
                fmt::format("gamedata played-event name={} hash={:#010x}", name, Murmur3(name))));
        }
    }
}

TEST_CASE("GameData trace reads nothing it cannot and stops logging at the cap",
          "[oot_gamedata_trace]") {
    const u64 entry = Site(Point::PlayedEvent);
    TraceEnvironment env;
    env.enabled = true;
    env.session.emplace(nullptr, Base, 1);
    env.Code(entry, Trace::Pacibsp);
    env.Code(entry + 4, MovzX9Seven);
    env.Code(entry + 8, Ret);
    env.Code(Stop, Svc0);
    // x0 points at unmapped memory: the line says so, the hook still resumes.
    RunAndCheck(env, entry, {Data, 0x12340001, 0x12340002, 0x12340003, 0x12340004, 0x12340005},
                ~0ULL, 7);
    REQUIRE(env.lines.size() == 1);
    REQUIRE(env.lines[0].starts_with("gamedata played-event name=? "));

    Trace::Session session{nullptr, Base, 2};
    bool last = false;
    for (u32 i = 0; i < Trace::Session::LineLimit; ++i) {
        REQUIRE(session.Take(Point::StepSet, last));
        REQUIRE(last == (i + 1 == Trace::Session::LineLimit));
    }
    REQUIRE_FALSE(session.Take(Point::StepSet, last));
    REQUIRE(session.Take(Point::StepApply, last));
}

TEST_CASE("GameData trace sessions follow the loaded main module", "[oot_gamedata_trace]") {
    Trace::Sessions sessions;
    int process_a = 0;
    int process_b = 0;
    const auto* a = reinterpret_cast<const Kernel::KProcess*>(&process_a);
    const auto* b = reinterpret_cast<const Kernel::KProcess*>(&process_b);
    REQUIRE_FALSE(sessions.Find(a));
    sessions.Reset(a, Base);
    const auto first = sessions.Find(a);
    REQUIRE(first);
    REQUIRE(first->base == Base);
    REQUIRE_FALSE(sessions.Find(b));
    sessions.Reset(b, 0); // another build: no hooks
    REQUIRE_FALSE(sessions.Find(a));
    REQUIRE_FALSE(sessions.Find(b));
}

TEST_CASE("GameData frame hook executes the replaced hint once", "[oot_gamedata_trace]") {
    TraceEnvironment env;
    env.enabled = true;
    env.session.emplace(nullptr, Base, 1);
    const u64 entry = Site(Point::Frame);
    env.Code(entry, Trace::Pacibsp);
    env.Code(entry + 4, MovzX9Seven);
    env.Code(entry + 8, Ret);
    env.Code(Stop, Svc0);
    RunAndCheck(env, entry, {}, ~u64{0}, 7);
    REQUIRE(env.breakpoints == 1);
    REQUIRE(env.lines.size() == 1);
    REQUIRE(env.lines[0].starts_with("gamedata frame"));
}

TEST_CASE("OoT frame redirects a real JIT and sentinel restores full context", "[oot_coop][oot_gamedata_trace]") {
    namespace Coop = Core::OotCoop;
    TraceEnvironment env;
    env.enabled = true;
    env.session.emplace(nullptr, Base, 1);
    const u64 frame = Site(Point::Frame);
    const u64 entry = Base + 0x3761720;
    constexpr u64 sentinel = Stop + 0x100;
    constexpr u64 manager = Data;
    constexpr u64 data = Data + 0x2000;
    constexpr u64 queue = Data + 0x3000;
    env.Code(frame, Trace::Pacibsp);
    env.Code(frame + 4, Svc0);
    env.Code(entry, 0xB40003C2); // cbz x2: nonzero reviewed key follows normal path
    env.Code(entry + 4, 0xAA0003E8);
    env.Code(entry + 8, Svc0); // Pause inside the synthetic setter to clobber its CPU context.
    env.Code(entry + 12, Ret);
    env.Code(sentinel, Trace::Breakpoint);
    env.Write32(manager + 0xC58, 1);
    env.Write64(manager + 0xC60, data);
    env.Write32(manager + 0xC68, 1);
    env.Write64(manager + 0xC70, queue);
    env.Write32(manager + 0xC78, 0);
    env.Write64(data, 101);
    env.Write64(data + 8, 0);
    env.Write64(queue, 0);
    Dynarmic::A64::Jit jit{Config(env)};
    env.jit = &jit;
    u64 tls = 0x1234, tls_ro = 0x5678;
    int exclusive_clears = 0;
    Coop::GuestBinding binding{Base, 0, 0, 0, sentinel, 0x20000000};
    for (u64 i = binding.stack - 0x4000; i < binding.stack; ++i) env.bytes[i] = 0;
    using Backend = Coop::GameDataBackend<Dynarmic::A64::Jit, TraceEnvironment>;
    Backend backend{jit, env, binding, tls, tls_ro, [&] { ++exclusive_clears; }};
    REQUIRE(backend.Bind(manager));
    const Coop::Change grant{Coop::Kind::Bit, "A", 1, 101, 1};
    REQUIRE(backend.ReadFlag(grant) == false);
    SECTION("queue full fails closed") {
        env.Write32(manager + 0xC78, 1);
        REQUIRE_FALSE(backend.Resolve(grant));
        REQUIRE(exclusive_clears == 0);
        return;
    }
    SECTION("changed table quarantines binding") {
        env.Write64(manager + 0xC70, queue + 8);
        REQUIRE_FALSE(backend.Bind(manager));
        REQUIRE(binding.invalid);
        REQUIRE_FALSE(backend.Resolve(grant));
        return;
    }
    SECTION("sentinel round trip") {}
    Coop::FrameDrain<Backend> drain{{true, {Coop::Kind::Bit}, {{Coop::Kind::Bit, 101}}}};
    REQUIRE(drain.Queue(grant));
    std::vector<std::string> logs;
    const auto log = [&](std::string_view line) { logs.emplace_back(line); };
    const auto send = [](Coop::Change, bool) { FAIL("Injected grant echoed"); };
    int frames = 0, returns = 0;
    env.on_breakpoint = [&](u64 pc) {
        if (pc == sentinel) {
            REQUIRE(drain.Sentinel(backend, {7, 9, 1, true}, pc, log) == Coop::ApplyEvent::Returned);
            ++returns;
        } else {
            std::string line;
            REQUIRE(Trace::Handle(*env.session, pc, jit, env, &line));
            if (pc == frame) {
                ++frames;
                REQUIRE(drain.Frame(backend, {7, 9, 1, true}, send, log) == Coop::ApplyEvent::Started);
            }
        }
    };
    for (size_t i = 0; i < 31; ++i) jit.SetRegister(i, 0x12340000 + i);
    for (size_t i = 0; i < 32; ++i) jit.SetVector(i, {0x45670000 + i, 0x78900000 + i});
    jit.SetSP(0x30000000);
    jit.SetPC(frame + 4);
    jit.SetPstate(0xA0000000);
    jit.SetFpcr(0x400000);
    jit.SetFpsr(0x8000001);
    auto expected = *backend.Capture();
    jit.SetPC(frame);
    jit.Run();
    REQUIRE(jit.GetPC() == entry + 12);
    REQUIRE(jit.GetRegister(0) == manager);
    REQUIRE(jit.GetRegister(1) == 1);
    REQUIRE(jit.GetRegister(2) == 101);
    REQUIRE(jit.GetRegister(30) == sentinel);
    REQUIRE(jit.GetSP() == binding.stack);
    for (size_t i = 0; i < 30; ++i) jit.SetRegister(i, i);
    for (size_t i = 0; i < 32; ++i) jit.SetVector(i, {i, i});
    jit.SetPstate(0);
    jit.SetFpcr(0);
    jit.SetFpsr(0);
    tls = tls_ro = 0;
    jit.Run();
    expected.pc = frame + 8; // Original continuation executed exactly one SVC.
    REQUIRE(*backend.Capture() == expected);
    REQUIRE(exclusive_clears == 2);
    REQUIRE(frames == 1);
    REQUIRE(returns == 1);
    REQUIRE(backend.ReadFlag(grant) == false); // Return alone is not a commit.
    env.bytes[data + 0xC] = 1;
    REQUIRE(drain.Frame(backend, {7, 9, 2, true}, send, log) == Coop::ApplyEvent::Committed);
    REQUIRE(drain.Suppress(grant));
}

TEST_CASE("GameData Bool64 reader hook resumes its zero extending load", "[oot_gamedata_trace]") {
    TraceEnvironment env;
    env.enabled = true;
    env.session.emplace(nullptr, Base, 1);
    const u64 entry = Site(Point::BitRead);
    env.Code(entry, 0xB94C5808);
    env.Code(entry + 4, MovzX9Seven);
    env.Code(entry + 8, Ret);
    env.Code(Stop, Svc0);
    env.Write32(Data + 0xC58, 0x80000001);
    RunAndCheck(env, entry, {Data, 0, 101, 0, 0, 0}, 0x80000001, 7);
    REQUIRE(env.breakpoints == 1);
    REQUIRE(env.lines.front().starts_with("gamedata bit-read mgr="));
}

TEST_CASE("OoT chest member hashes map to reviewed backing Bools", "[oot_coop]") {
    namespace Coop = Core::OotCoop;
    REQUIRE(Coop::ChestMembers.size() == 206);
    REQUIRE(Murmur3("4555665598794293658") == 0xB3DEDFE4);
    REQUIRE(Coop::ChestBool(0xB3DEDFE4) == 0xC8643846);
    REQUIRE(Coop::ChestBool(0x5A95678A) == 0x467020FD);
    REQUIRE_FALSE(Coop::ChestBool(0));
    REQUIRE_FALSE(Coop::IsChestBool(0x3F38F9B9A399E19A));
    std::set<u32> members, keys;
    for (const auto& [member, key] : Coop::ChestMembers) {
        REQUIRE(members.insert(member).second);
        REQUIRE(keys.insert(key).second);
        REQUIRE(Coop::ChestBool(member) == key);
    }
}

TEST_CASE("OoT flat Bool polling and deferred chest grants", "[oot_coop]") {
    namespace Coop = Core::OotCoop;
    constexpr u32 key = 0xC8643846;
    constexpr u64 manager = Data, data = Data + 0x2000, hashes = Data + 0x3000;
    constexpr u64 queue = Data + 0x4000, sentinel = Stop + 0x100;
    constexpr u64 entry = Base + 0x682B20;
    TraceEnvironment env;
    for (u64 i = data; i < data + 0x30; ++i) env.bytes[i] = 0;
    for (u64 i = hashes; i < hashes + 8 * 8; ++i) env.bytes[i] = 0;
    env.Write64(Base + 0x4E842A8, manager);
    env.Write32(manager + 0x10, 2);
    env.Write64(manager + 0x18, data);
    env.Write64(manager + 0x20, hashes);
    env.Write32(manager + 0x2C, 8);
    env.Write32(manager + 0x30, 1);
    env.Write64(manager + 0x38, queue);
    env.Write32(manager + 0x40, 0);
    env.Write64(queue, 0);
    // Force collision and wraparound: key % 8 == 6, found at slot 0, record index 1.
    env.Write32(hashes + 6 * 8, 1);
    env.Write32(hashes + 7 * 8, 2);
    env.Write32(hashes, key);
    env.Write32(hashes + 4, 1);
    env.Code(entry, 0xD503237F);
    env.Code(entry + 4, 0xD100C3FF);
    env.Code(entry + 8, 0xA9017BFD);
    env.Code(entry + 12, 0xF90013F3);
    env.Code(sentinel, Trace::Breakpoint);
    Coop::GuestBinding binding{Base, 0, 0, 0, sentinel, 0x20000000};
    for (u64 i = binding.stack - 0x4000; i < binding.stack; ++i) env.bytes[i] = 0;
    Dynarmic::A64::Jit jit{Config(env)};
    u64 tls = 123, tls_ro = 456;
    int clears = 0;
    using Backend = Coop::GameDataBackend<Dynarmic::A64::Jit, TraceEnvironment>;
    Backend backend{jit, env, binding, tls, tls_ro, [&] { ++clears; }};
    const Coop::Change grant{Coop::Kind::Bool, "A", 1, key, 1};
    REQUIRE(backend.BindFromCell()); // No Bool64 table or accessor call exists in this fixture.
    REQUIRE(backend.ReadFlag(grant) == false);
    Coop::FrameDrain<Backend> drain{{true, {Coop::Kind::Bool}, {{Coop::Kind::Bool, key}}}};
    std::vector<Coop::Change> sent;
    std::vector<std::string> logs;
    const auto send = [&](Coop::Change change, bool) { sent.push_back(change); };
    const auto log = [&](std::string_view line) { logs.emplace_back(line); };
    drain.Frame(backend, {7, 9, 1, true}, send, log);
    REQUIRE(drain.BaselineReady());
    REQUIRE(sent.empty());
    SECTION("local flip observed exactly once and rollback quarantines") {
        env.bytes[data + 0x18 + 0x16] = 1;
        drain.Frame(backend, {7, 9, 2, true}, send, log);
        drain.Frame(backend, {7, 9, 3, true}, send, log);
        REQUIRE(sent.size() == 1);
        REQUIRE(sent[0].kind == Coop::Kind::Bool);
        REQUIRE(sent[0].key == key);
        env.bytes[data + 0x18 + 0x16] = 0;
        REQUIRE(drain.Frame(backend, {7, 9, 4, true}, send, log) == Coop::ApplyEvent::Quarantined);
    }
    SECTION("grant shape, full restoration, deferred commit and echo suppression") {
        for (size_t i = 0; i < 31; ++i) jit.SetRegister(i, 100 + i);
        for (size_t i = 0; i < 32; ++i) jit.SetVector(i, {200 + i, 300 + i});
        jit.SetPC(Base + 0x11E7A54);
        jit.SetSP(0x30000000);
        jit.SetPstate(0xA0000000);
        jit.SetFpcr(0x400000);
        jit.SetFpsr(0x8000001);
        const auto saved = *backend.Capture();
        REQUIRE(drain.Queue(grant));
        REQUIRE(drain.Frame(backend, {7, 9, 2, true}, send, log) == Coop::ApplyEvent::Started);
        REQUIRE(jit.GetPC() == entry);
        REQUIRE(jit.GetRegister(0) == manager);
        REQUIRE(jit.GetRegister(1) == 1);
        REQUIRE(jit.GetRegister(2) == key);
        REQUIRE(jit.GetRegister(30) == sentinel);
        REQUIRE(jit.GetSP() == binding.stack);
        for (size_t i = 0; i < 31; ++i) jit.SetRegister(i, 0);
        for (size_t i = 0; i < 32; ++i) jit.SetVector(i, {0, 0});
        jit.SetPstate(0); jit.SetFpcr(0); jit.SetFpsr(0);
        tls = tls_ro = 0;
        REQUIRE(drain.Sentinel(backend, {7, 9, 2, true}, sentinel, log) == Coop::ApplyEvent::Returned);
        REQUIRE(*backend.Capture() == saved);
        REQUIRE(clears == 2);
        REQUIRE(drain.Frame(backend, {7, 9, 3, true}, send, log) == Coop::ApplyEvent::Blocked);
        env.bytes[data + 0x18 + 0x16] = 1;
        REQUIRE(drain.Frame(backend, {7, 9, 4, true}, send, log) == Coop::ApplyEvent::Committed);
        drain.Frame(backend, {7, 9, 5, true}, send, log);
        REQUIRE(sent.empty());
        REQUIRE(drain.Suppress(grant));
        REQUIRE_FALSE(drain.Queue(grant));
        for (const auto event : {"applied", "suppressed"}) {
            REQUIRE(std::count_if(logs.begin(), logs.end(), [&](const auto& line) {
                return line.find(std::string{event} + " class=bool key=00000000c8643846") !=
                       std::string::npos;
            }) == 1);
        }
    }
    SECTION("full queue blocks") {
        env.Write32(manager + 0x40, 1);
        REQUIRE_FALSE(backend.Resolve(grant));
    }
    SECTION("wrong setter word blocks") {
        env.Code(entry + 12, 0);
        REQUIRE_FALSE(backend.Resolve(grant));
    }
    SECTION("bad index blocks") {
        env.Write32(hashes + 4, 2);
        REQUIRE_FALSE(backend.ReadFlag(grant));
    }
    SECTION("non-boolean byte blocks") {
        env.bytes[data + 0x18 + 0x16] = 2;
        REQUIRE_FALSE(backend.ReadFlag(grant));
    }
    SECTION("changed table quarantines") {
        env.Write32(manager + 0x2C, 4);
        REQUIRE_FALSE(backend.ReadFlag(grant));
        REQUIRE(binding.invalid);
    }
    SECTION("an unhandled manager cell change still quarantines the backend") {
        env.Write64(Base + 0x4E842A8, 0);
        REQUIRE_FALSE(backend.BindFromCell());
        REQUIRE(binding.invalid);
    }
    SECTION("a moved or cleared manager re-binds the baseline instead of quarantining") {
        REQUIRE_FALSE(backend.Moved());
        env.bytes[data + 0x18 + 0x16] = 1;
        drain.Frame(backend, {7, 9, 2, true}, send, log);
        REQUIRE(sent.size() == 1);
        // Back to the title screen: the cell is cleared.
        env.Write64(Base + 0x4E842A8, 0);
        REQUIRE(backend.Moved());
        REQUIRE(drain.Rebind(backend, log));
        REQUIRE_FALSE(binding.invalid);
        REQUIRE(binding.manager == 0);
        REQUIRE(binding.sentinel == sentinel);
        REQUIRE_FALSE(backend.Moved());
        REQUIRE_FALSE(backend.BindFromCell());
        REQUIRE_FALSE(drain.BaselineReady());
        // A save loads into a new manager whose tables read the key false: no rollback.
        constexpr u64 moved = Data + 0x5000, moved_data = Data + 0x6000;
        for (u64 i = 0; i < 0x48; ++i)
            if (env.bytes.contains(manager + i)) env.bytes[moved + i] = env.bytes.at(manager + i);
        for (u64 i = moved_data; i < moved_data + 0x30; ++i) env.bytes[i] = 0;
        env.Write64(moved + 0x18, moved_data);
        env.Write64(Base + 0x4E842A8, moved);
        REQUIRE(backend.BindFromCell());
        REQUIRE(drain.Frame(backend, {7, 9, 3, true}, send, log) == Coop::ApplyEvent::None);
        REQUIRE(drain.BaselineReady());
        REQUIRE(binding.bool_data == moved_data);
        REQUIRE_FALSE(backend.Moved());
        REQUIRE(sent.size() == 1);
        env.bytes[moved_data + 0x18 + 0x16] = 1;
        drain.Frame(backend, {7, 9, 4, true}, send, log);
        REQUIRE(sent.size() == 2); // A new epoch's local open is observed again.
        // Same manager, reallocated table: also a move.
        env.Write64(moved + 0x18, data);
        REQUIRE(backend.Moved());
        REQUIRE(std::none_of(logs.begin(), logs.end(), [](const auto& line) {
            return line.find("quarantined") != std::string::npos;
        }));
    }
    SECTION("a grant awaiting commit is re-read against the moved tables") {
        REQUIRE(drain.Queue(grant));
        REQUIRE(drain.Frame(backend, {7, 9, 2, true}, send, log) == Coop::ApplyEvent::Started);
        REQUIRE(drain.Sentinel(backend, {7, 9, 2, true}, sentinel, log) ==
                Coop::ApplyEvent::Returned);
        env.Write64(Base + 0x4E842A8, 0);
        REQUIRE(backend.Moved());
        REQUIRE(drain.Rebind(backend, log));
        env.Write64(Base + 0x4E842A8, manager);
        REQUIRE(backend.BindFromCell());
        env.bytes[data + 0x18 + 0x16] = 1; // The new epoch already holds it: no second call.
        REQUIRE(drain.Frame(backend, {7, 9, 3, true}, send, log) == Coop::ApplyEvent::Committed);
        REQUIRE(sent.empty());
        REQUIRE(drain.Suppress(grant));
    }
    SECTION("a move while a synthetic call is in flight quarantines") {
        REQUIRE(drain.Queue(grant));
        REQUIRE(drain.Frame(backend, {7, 9, 2, true}, send, log) == Coop::ApplyEvent::Started);
        env.Write64(Base + 0x4E842A8, 0);
        REQUIRE(backend.Moved());
        REQUIRE_FALSE(drain.Rebind(backend, log));
        REQUIRE(drain.Frame(backend, {7, 9, 3, true}, send, log) ==
                Coop::ApplyEvent::Quarantined);
    }
}

TEST_CASE("Chest trace sites preserve hints and report keys", "[oot_gamedata_trace]") {
    TraceEnvironment env;
    Dynarmic::A64::Jit jit{Config(env)};
    jit.SetRegister(0, Data);
    jit.SetRegister(1, 1);
    jit.SetRegister(3, 0xB3DEDFE4);
    jit.SetRegister(8, Data + 0x100);
    env.Write32(Data + 0x108, 0xB3DEDFE4);
    env.Write64(Data + 0x188, 0x3F38F9B9A399E19A);
    for (const auto point : {Point::StructBoolSet, Point::ActorFlagSet}) {
        REQUIRE(Trace::Identify(Base, Site(point), Trace::Pacibsp) == point);
        REQUIRE_FALSE(Trace::CoopPoint(point));
        const auto before = jit.GetRegisters();
        Trace::Resume(point, jit, env);
        REQUIRE(jit.GetRegisters() == before);
        const auto line = Trace::Describe(point, Base, jit, env);
        REQUIRE(line.find("b3dedfe4") != std::string::npos);
        REQUIRE(line.find("value=1") != std::string::npos);
    }
    REQUIRE(Trace::Identify(Base, Base + 0x838964, 0xB94C5808) == Point::BitRead);
    REQUIRE(Trace::Identify(Base, Base + 0xF07134, 0xAA0003E8) == Point::BitSet);
    // Co-op without tracing traps only the frame boundary, never the hot Bool64 accessors.
    REQUIRE(Trace::CoopPoint(Point::Frame));
    for (const auto point : {Point::BitRead, Point::BitSet, Point::StepSet, Point::StepApply,
                             Point::PlayedEvent, Point::None})
        REQUIRE_FALSE(Trace::CoopPoint(point));
}

TEST_CASE("OoT co-op rupee grant calls the wallet entry with the pouch manager", "[oot_coop]") {
    namespace Coop = Core::OotCoop;
    constexpr u64 manager = Data, data = Data + 0x2000, hashes = Data + 0x3000;
    constexpr u64 queue = Data + 0x4000, sentinel = Stop + 0x100, pouch = Data + 0x8000;
    constexpr u64 entry = Base + Coop::RupeeEntry;
    TraceEnvironment env;
    for (u64 i = data; i < data + 0x30; ++i) env.bytes[i] = 0;
    for (u64 i = hashes; i < hashes + 8 * 8; ++i) env.bytes[i] = 0;
    env.Write64(Base + 0x4E842A8, manager);
    env.Write32(manager + 0x10, 2);
    env.Write64(manager + 0x18, data);
    env.Write64(manager + 0x20, hashes);
    env.Write32(manager + 0x2C, 8);
    env.Write32(manager + 0x30, 1);
    env.Write64(manager + 0x38, queue);
    env.Write32(manager + 0x40, 0);
    env.Write64(queue, 0);
    env.Write64(Base + Coop::PouchCell, pouch);
    env.Write64(pouch + 0x3BD4, 0);
    env.Code(entry, 0x52877A88);
    env.Code(entry + 4, 0x8B080008);
    env.Code(entry + 8, 0xB8210108);
    env.Code(entry + 12, 0xB93BD802);
    env.Code(sentinel, Trace::Breakpoint);
    Coop::GuestBinding binding{Base, 0, 0, 0, sentinel, 0x20000000};
    for (u64 i = binding.stack - 0x4000; i < binding.stack; ++i) env.bytes[i] = 0;
    Dynarmic::A64::Jit jit{Config(env)};
    u64 tls = 123, tls_ro = 456;
    int clears = 0;
    using Backend = Coop::GameDataBackend<Dynarmic::A64::Jit, TraceEnvironment>;
    Backend backend{jit, env, binding, tls, tls_ro, [&] { ++clears; }};
    REQUIRE(backend.BindFromCell());
    const Coop::Change minus{Coop::Kind::Rupee, "B", 1, Coop::WalletHash, -5};
    SECTION("call shape, restoration and commit on return") {
        Coop::FrameDrain<Backend> drain{{true, {Coop::Kind::Rupee}, {}}};
        std::vector<std::string> logs;
        const auto send = [](Coop::Change, bool) { FAIL("a delta is never observed"); };
        const auto log = [&](std::string_view line) { logs.emplace_back(line); };
        drain.Frame(backend, {7, 9, 1, true}, send, log);
        REQUIRE(drain.BaselineReady());
        jit.SetPC(Base + 0x11E7A54);
        jit.SetSP(0x30000000);
        const auto saved = *backend.Capture();
        REQUIRE(drain.Queue(minus));
        REQUIRE(drain.Frame(backend, {7, 9, 2, true}, send, log) == Coop::ApplyEvent::Started);
        REQUIRE(jit.GetPC() == entry);
        REQUIRE(jit.GetRegister(0) == pouch);
        REQUIRE(jit.GetRegister(1) == 0xFFFFFFFBU); // w1 = -5
        REQUIRE(jit.GetRegister(2) == Coop::RupeeReason);
        REQUIRE(jit.GetRegister(30) == sentinel);
        REQUIRE(jit.GetSP() == binding.stack);
        REQUIRE(drain.Sentinel(backend, {7, 9, 2, true}, sentinel, log) == Coop::ApplyEvent::Returned);
        REQUIRE(*backend.Capture() == saved);
        REQUIRE(drain.Frame(backend, {7, 9, 3, true}, send, log) == Coop::ApplyEvent::Committed);
        REQUIRE(std::count_if(logs.begin(), logs.end(), [](const auto& line) {
            return line.find("applied class=rupee key=00000000a77921d7 value=-5 path=call") !=
                   std::string::npos;
        }) == 1);
        REQUIRE(drain.Queue({Coop::Kind::Rupee, "B", 2, Coop::WalletHash, -5}));
    }
    SECTION("a changed wallet entry word blocks") {
        env.Code(entry + 8, 0);
        REQUIRE_FALSE(backend.Resolve(minus));
    }
    SECTION("no pouch manager yet blocks") {
        env.Write64(Base + Coop::PouchCell, 0);
        REQUIRE_FALSE(backend.Resolve(minus));
    }
    SECTION("an unmapped pending field blocks") {
        env.Write64(Base + Coop::PouchCell, pouch + 0x100000);
        REQUIRE_FALSE(backend.Resolve(minus));
    }
    SECTION("a missing sentinel breakpoint blocks") {
        env.Code(sentinel, 0);
        REQUIRE_FALSE(backend.Resolve(minus));
    }
}
