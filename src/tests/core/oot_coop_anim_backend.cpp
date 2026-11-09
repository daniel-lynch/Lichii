// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <bit>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/arm/dynarmic/oot_coop_runtime.h"
#include "dynarmic/interface/A64/a64.h"

namespace {

// Synthetic guest layout: a callee, the sentinel brk, and the interrupted caller's continuation.
constexpr u64 Entry = 0x10001000;
constexpr u64 Sentinel = 0x10002000;
constexpr u64 Caller = 0x10003000;
constexpr u64 StackTop = 0x20000000;

/// Code the real JIT fetches; no data access is expected (any data access fails the test).
class CallEnvironment final : public Dynarmic::A64::UserCallbacks {
public:
    Dynarmic::A64::Jit* jit{};
    std::map<u64, u32> code;
    std::function<void(u64)> on_breakpoint;

    std::optional<u32> MemoryReadCode(u64 address) override {
        const auto it = code.find(address);
        return it == code.end() ? std::nullopt : std::optional<u32>{it->second};
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
        REQUIRE(on_breakpoint);
        on_breakpoint(pc);
    }
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override {
        return 1000;
    }
    u64 GetCNTPCT() override {
        return 0;
    }
};

/// The backend's view of guest memory: only the private call stack exists.
struct StackMemory {
    std::vector<u8> bytes = std::vector<u8>(0x4000);
    bool IsValidVirtualAddressRange(u64 address, u64 size) const {
        return address >= StackTop - bytes.size() && size <= StackTop - address;
    }
    void Write64(u64 address, u64 value) {
        REQUIRE(IsValidVirtualAddressRange(address, sizeof(value)));
        std::memcpy(bytes.data() + (address - (StackTop - bytes.size())), &value, sizeof(value));
    }
};

} // namespace

// The ghost's frame and rate setters take their float in s0. This runs the actual applier
// (Capture, Redirect, Restore) against the real JIT: guest code must see the argument in s0,
// may clobber integer, SIMD and FP-status state, and the caller must get all of it back.
TEST_CASE("OoT animation native calls deliver s0 to real guest code and restore FP/SIMD state",
          "[core][oot_coop][oot_coop_anim]") {
    namespace Coop = Core::OotCoop;
    for (const float argument : {0.0f, 7.25f}) {
        CAPTURE(argument);
        CallEnvironment env;
        env.code[Entry + 0] = 0x1E260009;  // fmov w9, s0       (what the callee received)
        env.code[Entry + 4] = 0x1E202800;  // fadd s0, s0, s0   (callee changes v0)
        env.code[Entry + 8] = 0x6F00E401;  // movi v1.2d, #0    (callee clobbers v1)
        env.code[Entry + 12] = 0xD28000EA; // movz x10, #7
        env.code[Entry + 16] = 0xD4000001; // svc #0            (pause inside the callee)
        env.code[Entry + 20] = 0xD65F03C0; // ret               (to x30 = sentinel)
        env.code[Sentinel] = Core::OotGameDataTrace::Breakpoint;
        env.code[Caller] = 0xD4000001;     // svc #0            (the interrupted caller resumes)
        Dynarmic::A64::UserConfig config;
        config.callbacks = &env;
        config.enable_cycle_counting = false;
        Dynarmic::A64::Jit jit{config};
        env.jit = &jit;

        for (size_t i = 0; i < 31; ++i) jit.SetRegister(i, 0x1234000000000000ULL + i);
        for (size_t i = 0; i < 32; ++i) {
            jit.SetVector(i, {0xFEDCBA9800000000ULL + i, 0x8765432100000000ULL + i});
        }
        jit.SetPC(Caller);
        jit.SetSP(StackTop - 0x1000);
        jit.SetPstate(0xA0000000);
        jit.SetFpcr(0x00C00000); // round toward zero: a non-default caller FPCR
        jit.SetFpsr(0x08000000); // QC set: a non-default caller FPSR

        StackMemory memory;
        u64 tls = 0x12345678, tls_ro = 0x87654321;
        Coop::GuestBinding binding;
        binding.base = 0x10000000;
        binding.sentinel = Sentinel;
        binding.stack = StackTop;
        const Core::OotGameDataTrace::Profile profile{};
        int host_exclusive_clears = 0;
        Coop::GameDataBackend<Dynarmic::A64::Jit, StackMemory> backend{
            jit, memory, binding, profile, tls, tls_ro, [&] { ++host_exclusive_clears; }};
        const auto saved = backend.Capture();
        REQUIRE(saved);

        // The ghost port's call shape: x0 actor, w1/w2 zero, s0 the float, a zeroed frame.
        Coop::ApplyTarget target;
        target.verified = true;
        target.entry = Entry;
        target.sentinel = Sentinel;
        target.stack = StackTop - Coop::ItemFrame;
        target.arguments = {0x62000000, 0, 0, 0, 0, 0, 0, 0};
        target.stores = {{target.stack, 0}, {target.stack + 8, 0}};
        target.s0_bits = std::bit_cast<u32>(argument);
        REQUIRE(backend.Redirect(target));
        jit.Run();

        // Inside the callee: the guest instruction read exactly our bits from s0.
        REQUIRE(jit.GetPC() == Entry + 20);
        REQUIRE(jit.GetRegister(9) == u64{std::bit_cast<u32>(argument)});
        REQUIRE(jit.GetRegister(0) == 0x62000000);
        REQUIRE(jit.GetRegister(1) == 0);
        REQUIRE(jit.GetRegister(2) == 0);
        REQUIRE(jit.GetRegister(30) == Sentinel);
        REQUIRE(jit.GetSP() == target.stack);
        REQUIRE(jit.GetVector(0)[0] == u64{std::bit_cast<u32>(argument + argument)});
        REQUIRE(jit.GetVector(1) == Dynarmic::A64::Vector{0, 0});
        REQUIRE(jit.GetVector(2) == saved->v[2]); // untouched lanes are the caller's
        jit.SetFpcr(0);                          // a callee may also change FP control/status
        jit.SetFpsr(0x9F);

        int returns = 0;
        env.on_breakpoint = [&](u64 pc) {
            REQUIRE(pc == Sentinel);
            REQUIRE(backend.Restore(*saved));
            ++returns;
        };
        jit.Run();
        REQUIRE(returns == 1);
        auto expected = *saved;
        expected.pc = Caller + 4; // the caller's continuation executed its one svc
        REQUIRE(*backend.Capture() == expected);
        REQUIRE(jit.GetFpcr() == 0x00C00000);
        REQUIRE(jit.GetFpsr() == 0x08000000);
        REQUIRE(host_exclusive_clears == 2); // Redirect and Restore each drop reservations
    }
}
