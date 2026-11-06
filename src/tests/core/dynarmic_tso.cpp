// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Upstream PR #161 on x86-64: ordered loads (LDAR/LDAXR/LDXR/LDAPR) become plain MOVs, DMB LD/ST
// emit nothing, and MRS CNTPCT_EL0 is an inline RDTSC scaling. These run real JITs over fastmem
// (the path the emulator uses) with UserConfig::x64_tso_memory on and off: concurrent exclusive
// increments must not lose an update, the ordered/exclusive forms must read and write the right
// bytes, and the inline counter must match WallClock::GetCNTPCT without calling back.

#if defined(__x86_64__) || defined(_M_X64)

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include <sys/mman.h>

#include <catch2/catch_test_macros.hpp>

#include "common/cpu_features.h"
#include "dynarmic/interface/A64/a64.h"
#include "dynarmic/interface/A64/config.h"
#include "dynarmic/interface/exclusive_monitor.h"

namespace {

constexpr std::size_t kAddressBits = 16;
constexpr std::size_t kMemSize = std::size_t{1} << kAddressBits;

struct GuestMemory {
    GuestMemory() {
        void* p = mmap(nullptr, kMemSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1,
                       0);
        REQUIRE(p != MAP_FAILED);
        base = static_cast<std::uint8_t*>(p);
    }
    ~GuestMemory() {
        munmap(base, kMemSize);
    }
    template <typename T>
    T Read(std::uint64_t a) const {
        T v;
        std::memcpy(&v, base + a, sizeof(T));
        return v;
    }
    template <typename T>
    void Write(std::uint64_t a, T v) {
        std::memcpy(base + a, &v, sizeof(T));
    }
    void WriteCode(std::uint64_t a, const std::vector<std::uint32_t>& code) {
        for (std::size_t i = 0; i < code.size(); ++i) {
            Write<std::uint32_t>(a + i * 4, code[i]);
        }
    }
    std::uint8_t* base{};
};

class Env final : public Dynarmic::A64::UserCallbacks {
public:
    using Vector = Dynarmic::A64::Vector;
    explicit Env(GuestMemory& mem_) : mem{mem_} {}

    GuestMemory& mem;
    Dynarmic::A64::Jit* jit{};
    std::uint64_t cntpct_calls{};
    bool raised{};

    template <typename T>
    bool Cas(std::uint64_t a, T value, T expected) {
        return __atomic_compare_exchange_n(reinterpret_cast<T*>(mem.base + a), &expected, value,
                                           false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }

    std::optional<std::uint32_t> MemoryReadCode(std::uint64_t a) override {
        return mem.Read<std::uint32_t>(a);
    }
    std::uint64_t MemoryRead(std::uint64_t a, std::size_t size) override {
        switch (size) {
        case 1: return mem.Read<std::uint8_t>(a);
        case 2: return mem.Read<std::uint16_t>(a);
        case 4: return mem.Read<std::uint32_t>(a);
        default: return mem.Read<std::uint64_t>(a);
        }
    }
    Vector MemoryRead128(std::uint64_t a) override {
        return mem.Read<Vector>(a);
    }
    void MemoryWrite(std::uint64_t a, std::uint64_t v, std::size_t size) override {
        switch (size) {
        case 1: return mem.Write<std::uint8_t>(a, static_cast<std::uint8_t>(v));
        case 2: return mem.Write<std::uint16_t>(a, static_cast<std::uint16_t>(v));
        case 4: return mem.Write<std::uint32_t>(a, static_cast<std::uint32_t>(v));
        default: return mem.Write<std::uint64_t>(a, v);
        }
    }
    void MemoryWrite128(std::uint64_t a, Vector v) override {
        mem.Write(a, v);
    }
    bool MemoryWriteExclusive8(std::uint64_t a, std::uint8_t v, std::uint8_t e) override {
        return Cas(a, v, e);
    }
    bool MemoryWriteExclusive16(std::uint64_t a, std::uint16_t v, std::uint16_t e) override {
        return Cas(a, v, e);
    }
    bool MemoryWriteExclusive32(std::uint64_t a, std::uint32_t v, std::uint32_t e) override {
        return Cas(a, v, e);
    }
    bool MemoryWriteExclusive64(std::uint64_t a, std::uint64_t v, std::uint64_t e) override {
        return Cas(a, v, e);
    }
    bool MemoryWriteExclusive128(std::uint64_t a, Vector v, Vector e) override {
        unsigned __int128 ev, vv;
        std::memcpy(&ev, &e, 16);
        std::memcpy(&vv, &v, 16);
        return __sync_bool_compare_and_swap(reinterpret_cast<unsigned __int128*>(mem.base + a), ev,
                                            vv);
    }
    void CallSVC(std::uint32_t) override {
        jit->HaltExecution();
    }
    void ExceptionRaised(std::uint64_t, Dynarmic::A64::Exception) override {
        raised = true;
        jit->HaltExecution();
    }
    void AddTicks(std::uint64_t) override {}
    std::uint64_t GetTicksRemaining() override {
        return 1'000'000'000;
    }
    std::uint64_t GetCNTPCT() override {
        ++cntpct_calls;
        return Common::g_wall_clock.GetCNTPCT();
    }
};

struct Core {
    Core(GuestMemory& mem, Dynarmic::ExclusiveMonitor& monitor, std::size_t id, bool tso,
         bool ignore_monitor, bool inline_cntpct)
        : env{mem} {
        Dynarmic::A64::UserConfig config{};
        config.callbacks = &env;
        config.processor_id = id;
        config.global_monitor = &monitor;
        config.fastmem_pointer = reinterpret_cast<std::uintptr_t>(mem.base);
        config.fastmem_address_space_bits = kAddressBits;
        config.silently_mirror_fastmem = true;
        config.recompile_on_fastmem_failure = false;
        config.fastmem_exclusive_access = true;
        config.recompile_on_exclusive_fastmem_failure = false;
        config.define_unpredictable_behaviour = true;
        config.wall_clock_cntpct = true;
        config.enable_cycle_counting = false;
        config.code_cache_size = 8 * 1024 * 1024;
        config.x64_tso_memory = tso;
        if (ignore_monitor) {
            config.optimizations |= Dynarmic::OptimizationFlag::Unsafe_IgnoreGlobalMonitor;
        }
        if (inline_cntpct && Common::g_wall_clock.IsNative()) {
            config.inline_cntpct_tsc_base = Common::g_wall_clock.base_rdtsc;
            config.inline_cntpct_tsc_factor =
                Common::g_wall_clock.cntpct_rdtsc_factor.load(std::memory_order_relaxed);
        }
        jit = std::make_unique<Dynarmic::A64::Jit>(config);
        env.jit = jit.get();
        jit->SetSP(kMemSize - 0x10);
        jit->SetPstate(0);
    }
    Env env;
    std::unique_ptr<Dynarmic::A64::Jit> jit;
};

// x0 -> u32 counter, x3 = iterations; x4 -> u64 counter, x5 = iterations; then SVC.
const std::vector<std::uint32_t> kIncrementLoops{
    0x885ffc01, // loop32: ldaxr w1, [x0]
    0x11000421, //         add w1, w1, #1
    0x8802fc01, //         stlxr w2, w1, [x0]
    0x35ffffa2, //         cbnz w2, loop32
    0xd1000463, //         sub x3, x3, #1
    0xb5ffff63, //         cbnz x3, loop32
    0xc85f7c81, // loop64: ldxr x1, [x4]
    0x91000421, //         add x1, x1, #1
    0xc8027c81, //         stxr w2, x1, [x4]
    0x35ffffa2, //         cbnz w2, loop64
    0xd10004a5, //         sub x5, x5, #1
    0xb5ffff65, //         cbnz x5, loop64
    0xd4000001, //         svc #0
};

// x7 -> 16 bytes (swapped by LDAXP/STLXP), x8 -> STLR target, x9 -> u8 then u16 counters.
const std::vector<std::uint32_t> kOrderedForms{
    0x88dffcea, // ldar w10, [x7]
    0xc8dffceb, // ldar x11, [x7]
    0x08dffcec, // ldarb w12, [x7]
    0x48dffced, // ldarh w13, [x7]
    0xd50339bf, // dmb ishld
    0xc89ffd0b, // stlr x11, [x8]
    0xd5033abf, // dmb ishst
    0xf8bfc10e, // ldapr x14, [x8]
    0xd5033bbf, // dmb ish
    0x085ffd2f, // ldaxrb w15, [x9]
    0x110005ef, // add w15, w15, #1
    0x0810fd2f, // stlxrb w16, w15, [x9]
    0x485ffd31, // ldaxrh w17, [x9]
    0x11000631, // add w17, w17, #1
    0x4812fd31, // stlxrh w18, w17, [x9]
    0xc87fd0f3, // ldaxp x19, x20, [x7]
    0xc835ccf4, // stlxp w21, x20, x19, [x7]
    0xd53be036, // mrs x22, cntpct_el0
    0xd53be037, // mrs x23, cntpct_el0
    0xd4000001, // svc #0
};

} // Anonymous namespace

TEST_CASE("x64 TSO: concurrent exclusive increments lose no update", "[dynarmic_tso]") {
    constexpr std::size_t kCores = 4;
    constexpr std::uint64_t kIterations = 200'000;
    for (const bool tso : {true, false}) {
        for (const bool ignore_monitor : {true, false}) {
            INFO("x64_tso_memory=" << tso << " ignore_global_monitor=" << ignore_monitor);
            GuestMemory mem;
            mem.WriteCode(0, kIncrementLoops);
            Dynarmic::ExclusiveMonitor monitor{kCores};
            std::vector<std::unique_ptr<Core>> cores;
            for (std::size_t i = 0; i < kCores; ++i) {
                cores.push_back(std::make_unique<Core>(mem, monitor, i, tso, ignore_monitor, true));
                auto& jit = *cores.back()->jit;
                jit.SetPC(0);
                jit.SetRegister(0, 0x1000);
                jit.SetRegister(3, kIterations);
                jit.SetRegister(4, 0x1008);
                jit.SetRegister(5, kIterations);
            }
            std::vector<std::thread> threads;
            for (auto& core : cores) {
                threads.emplace_back([&core] { core->jit->Run(); });
            }
            for (auto& t : threads) {
                t.join();
            }
            for (auto& core : cores) {
                REQUIRE_FALSE(core->env.raised);
                REQUIRE(core->jit->GetRegister(3) == 0);
                REQUIRE(core->jit->GetRegister(5) == 0);
            }
            REQUIRE(mem.Read<std::uint32_t>(0x1000) == kCores * kIterations);
            REQUIRE(mem.Read<std::uint64_t>(0x1008) == kCores * kIterations);
        }
    }
}

TEST_CASE("x64 TSO: ordered and exclusive forms read and write the right bytes",
          "[dynarmic_tso]") {
    for (const bool tso : {true, false}) {
        for (const bool inline_cntpct : {true, false}) {
            INFO("x64_tso_memory=" << tso << " inline_cntpct=" << inline_cntpct);
            GuestMemory mem;
            mem.WriteCode(0, kOrderedForms);
            mem.Write<std::uint64_t>(0x2000, 0x1122334455667788ULL);
            mem.Write<std::uint64_t>(0x2008, 0x99AABBCCDDEEFF00ULL);
            mem.Write<std::uint8_t>(0x3000, 0xFE);
            mem.Write<std::uint16_t>(0x3002, 0xFFFE);
            Dynarmic::ExclusiveMonitor monitor{1};
            Core core{mem, monitor, 0, tso, true, inline_cntpct};
            auto& jit = *core.jit;
            jit.SetPC(0);
            jit.SetRegister(7, 0x2000);
            jit.SetRegister(8, 0x2800);
            jit.SetRegister(9, 0x3000);
            const auto before = static_cast<std::uint64_t>(Common::g_wall_clock.GetCNTPCT());
            jit.Run();
            const auto after = static_cast<std::uint64_t>(Common::g_wall_clock.GetCNTPCT());
            REQUIRE_FALSE(core.env.raised);
            REQUIRE(jit.GetRegister(10) == 0x55667788ULL);
            REQUIRE(jit.GetRegister(11) == 0x1122334455667788ULL);
            REQUIRE(jit.GetRegister(12) == 0x88ULL);
            REQUIRE(jit.GetRegister(13) == 0x7788ULL);
            REQUIRE(jit.GetRegister(14) == 0x1122334455667788ULL);
            // LDAXRB/STLXRB on 0xFE -> 0xFF; LDAXRH reads the halfword at x9 (0x00FF after the byte
            // store) and writes 0x0100.
            REQUIRE(jit.GetRegister(16) == 0);
            REQUIRE(jit.GetRegister(18) == 0);
            REQUIRE(mem.Read<std::uint16_t>(0x3000) == 0x0100);
            // LDAXP/STLXP: the pair swapped in place (STLR's copy at x8 is untouched).
            REQUIRE(jit.GetRegister(19) == 0x1122334455667788ULL);
            REQUIRE(jit.GetRegister(20) == 0x99AABBCCDDEEFF00ULL);
            REQUIRE(jit.GetRegister(21) == 0);
            REQUIRE(mem.Read<std::uint64_t>(0x2000) == 0x99AABBCCDDEEFF00ULL);
            REQUIRE(mem.Read<std::uint64_t>(0x2008) == 0x1122334455667788ULL);
            REQUIRE(mem.Read<std::uint64_t>(0x2800) == 0x1122334455667788ULL);
            // The counter: monotonic, and bracketed by the C++ reads (the inline RDTSC is not
            // LFENCEd, so allow it a tick of slack).
            const std::uint64_t c0 = jit.GetRegister(22);
            const std::uint64_t c1 = jit.GetRegister(23);
            REQUIRE(c0 <= c1);
            REQUIRE(c0 + 1 >= before);
            REQUIRE(c1 <= after + 1);
            if (inline_cntpct && Common::g_wall_clock.IsNative()) {
                REQUIRE(core.env.cntpct_calls == 0);
            } else {
                REQUIRE(core.env.cntpct_calls == 2);
            }
        }
    }
}

#endif
