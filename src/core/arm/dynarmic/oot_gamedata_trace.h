// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/nx2_event_trace.h"

namespace Kernel {
class KProcess;
}

/// EDEN_NX2_GAMEDATA_TRACE: Ocarina of Time's quest-step changes and HavePlayedEvent writes, for
/// randomizer research. A pure observer that never writes guest memory: the JIT is handed a
/// `brk #0` in place of the original instruction at verified sites (DynarmicCallbacks64::
/// MemoryReadCode, the same way as Core::WindowTrace), and the breakpoint's handler logs the
/// arguments and then does exactly what the replaced instruction does (Resume) before execution
/// continues at the next instruction. Guest data reads of the code still see the original words.
///
/// Sites (main 1.1.0, build id E0F3CCFAF50065AE..., offsets from the main module's base), from
/// static analysis on 2026-11-06 (findings-silver-gauntlets-adult.md, addendum 1 and 4); the
/// register layouts are NOT yet verified at run time:
/// - StepSet: FUN_7102cf54a0(mgr, challenge hash, step hash, ui, ?, force), the quest step setter
///   (EventExecuteProgressChallengeStep and friends). Hooked at +4 (`mov w8, #1`) because its
///   first instruction is a load; x0-x5 are still the arguments there.
/// - StepApply: FUN_7102cf1830(challenge + 0x18, ?), where the challenge manager applies a pending
///   step (also once per challenge when a save loads). `pacibsp`, a no-op in the JIT.
/// - PlayedEvent: vt_7104b21598::vslot3, the generic HavePlayedEvent.<event> = true setter; the
///   task's event name is at *(*(x0 + 0x10) + 8), its length (with the terminator) at
///   *(*(x0 + 0x10) + 0x10). `pacibsp`.
namespace Core::OotGameDataTrace {

inline constexpr u64 ProgramId = 0x0400486024DEC000ULL;
/// The main NSO's build id (its first 20 bytes) the sites belong to.
inline constexpr std::string_view BuildId = "E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348";

[[nodiscard]] inline bool Enabled() {
    return Common::Nx2EventTrace::GameDataTarget().On();
}

enum class Point : u8 {
    None,
    StepSet,
    StepApply,
    PlayedEvent,
    Frame,
    BitRead,
    BitSet,
    StructBoolSet,
    ActorFlagSet,
};
inline constexpr size_t PointCount = 9;

struct Site {
    Point point;
    u64 offset;      ///< From the main module's base (0x7100000000 in the Ghidra project)
    u32 instruction; ///< The original instruction there; any other word means no hook
};

inline constexpr u32 MovW8One = 0x52800028; // mov w8, #1
inline constexpr u32 Pacibsp = 0xD503237F;  // pacibsp (a hint: dynarmic runs it as a no-op)
inline constexpr u32 Breakpoint = 0xD4200000; // brk #0

inline constexpr std::array<Site, 10> Sites{{
    {Point::StepSet, 0x2CF54A4, MovW8One},
    {Point::StepApply, 0x2CF1830, Pacibsp},
    {Point::PlayedEvent, 0x2BDC4E0, Pacibsp},
    {Point::Frame, 0x11E7A50, Pacibsp},
    {Point::BitRead, 0x3761684, 0xB94C5808}, // ldr w8, [x0, #0xc58]
    {Point::BitSet, 0x3761724, 0xAA0003E8}, // mov x8, x0
    {Point::BitRead, 0x838964, 0xB94C5808},
    {Point::BitSet, 0xF07134, 0xAA0003E8},
    {Point::StructBoolSet, 0x608A10, Pacibsp},
    {Point::ActorFlagSet, 0xF07048, Pacibsp},
}};

constexpr bool CoopPoint(Point point) {
    return point == Point::Frame || point == Point::BitRead || point == Point::BitSet;
}

/// The trace point at `address` whose original instruction is `instruction`, or None.
constexpr Point Identify(u64 base, u64 address, u32 instruction) {
    if (base == 0 || address < base) {
        return Point::None;
    }
    for (const Site& site : Sites) {
        if (address - base == site.offset && instruction == site.instruction) {
            return site.point;
        }
    }
    return Point::None;
}

/// What the JIT translates at `address`: a breakpoint at a trace point, the original otherwise.
constexpr u32 InstructionFor(u64 base, u64 address, u32 instruction) {
    return Identify(base, address, instruction) == Point::None ? instruction : Breakpoint;
}

/// Does what the replaced instruction would have: the JIT has already moved the PC past it.
template <typename Jit, typename Memory>
void Resume(Point point, Jit& jit, Memory& memory) {
    switch (point) {
    case Point::StepSet:
        jit.SetRegister(8, 1); // mov w8, #1 (a W write clears the upper half)
        break;
    case Point::BitRead:
        jit.SetRegister(8, memory.Read32(jit.GetRegister(0) + 0xC58));
        break;
    case Point::BitSet:
        jit.SetRegister(8, jit.GetRegister(0));
        break;
    case Point::StructBoolSet:
    case Point::ActorFlagSet:
    case Point::Frame:
    case Point::StepApply:
    case Point::PlayedEvent: // pacibsp: nothing
    case Point::None:
        break;
    }
}

constexpr std::string_view Name(Point point) {
    switch (point) {
    case Point::StepSet:
        return "step-set";
    case Point::StepApply:
        return "step-apply";
    case Point::PlayedEvent:
        return "played-event";
    case Point::StructBoolSet: return "struct-bool-set";
    case Point::ActorFlagSet: return "actor-flag-set";
    case Point::Frame: return "frame";
    case Point::BitRead: return "bit-read";
    case Point::BitSet: return "bit-set";
    case Point::None:
        break;
    }
    return "none";
}

/// One process's trace: the main module's base and how many lines each point has written.
class Session {
public:
    static constexpr u32 LineLimit = 4096; ///< Per point and session; the hooks keep resuming

    Session(const Kernel::KProcess* owner_, u64 base_, u64 id_)
        : owner{owner_}, base{base_}, id{id_} {}

    /// Whether this hit may log: true LineLimit times per point. `last` is set on the last one.
    bool Take(Point point, bool& last) {
        const u32 count = lines[static_cast<size_t>(point)].fetch_add(1, std::memory_order_relaxed);
        last = count + 1 == LineLimit;
        return count < LineLimit;
    }

    const Kernel::KProcess* const owner;
    const u64 base;
    const u64 id;

private:
    std::array<std::atomic<u32>, PointCount> lines{};
};

class Sessions {
public:
    /// The main module of `owner` loaded at `base` (0: not the traced build, no hooks).
    void Reset(const Kernel::KProcess* owner, u64 base) {
        auto session =
            base != 0 ? std::make_shared<Session>(owner, base, next_id.fetch_add(1)) : nullptr;
        std::scoped_lock lock{mutex};
        current = std::move(session);
    }

    std::shared_ptr<Session> Find(const Kernel::KProcess* owner) const {
        std::scoped_lock lock{mutex};
        return current && current->owner == owner ? current : nullptr;
    }

private:
    mutable std::mutex mutex;
    std::shared_ptr<Session> current;
    std::atomic<u64> next_id{1};
};

inline Sessions sessions;

namespace Detail {
template <typename Memory>
std::optional<u32> Read32(Memory& memory, u64 address) {
    if (!memory.IsValidVirtualAddressRange(address, sizeof(u32))) {
        return std::nullopt;
    }
    return memory.Read32(address);
}

template <typename Memory>
std::optional<u64> Read64(Memory& memory, u64 address) {
    if (!memory.IsValidVirtualAddressRange(address, sizeof(u64))) {
        return std::nullopt;
    }
    return memory.Read64(address);
}

inline std::string Hash(std::optional<u32> hash) {
    return hash ? Common::Nx2EventTrace::DescribeHash(*hash) : std::string{"?"};
}

inline std::string Index(std::optional<u32> value) {
    return value ? fmt::format("{}", *value) : std::string{"?"};
}

/// The current step hash of challenge `challenge` in the manager at `manager`: three arrays of
/// 0x160-byte records ({count, data} at +0x40/+0x48, +0x50/+0x58, +0x60/+0x68), name hash at +8,
/// current step hash at +0x10 (FUN_7102cf54a0's own search).
template <typename Memory>
std::optional<u32> CurrentStep(Memory& memory, u64 manager, u32 challenge) {
    constexpr u32 MaxRecords = 4096;
    for (const u64 table : {u64{0x40}, u64{0x50}, u64{0x60}}) {
        const auto count = Read32(memory, manager + table);
        const auto data = Read64(memory, manager + table + 8);
        if (!count || !data || *count > MaxRecords) {
            continue;
        }
        for (u32 i = 0; i < *count; ++i) {
            const u64 record = *data + u64{i} * 0x160;
            if (Read32(memory, record + 8) == challenge) {
                return Read32(memory, record + 0x10);
            }
        }
    }
    return std::nullopt;
}
} // namespace Detail

/// The line for a hit, read from the registers and guest memory (reads only, each one checked
/// first, so a wrong layout prints "?" instead of touching unmapped memory).
template <typename Jit, typename Memory>
std::string Describe(Point point, u64 base, Jit& jit, Memory& memory) {
    using namespace Detail;
    const u64 x0 = jit.GetRegister(0);
    const u64 lr = jit.GetRegister(30);
    const std::string caller = lr >= base ? fmt::format("main+{:#x}", lr - base)
                                          : fmt::format("{:#x}", lr);
    switch (point) {
    case Point::StepSet: {
        const u32 challenge = static_cast<u32>(jit.GetRegister(1));
        const u32 step = static_cast<u32>(jit.GetRegister(2));
        return fmt::format("gamedata step-set challenge={} step={} current={} ui={} arg4={} "
                           "force={} mgr={:#x} lr={}",
                           Common::Nx2EventTrace::DescribeHash(challenge),
                           Common::Nx2EventTrace::DescribeHash(step),
                           Hash(CurrentStep(memory, x0, challenge)),
                           static_cast<u32>(jit.GetRegister(3)),
                           static_cast<u32>(jit.GetRegister(4)),
                           static_cast<u32>(jit.GetRegister(5)), x0, caller);
    }
    case Point::StepApply: {
        // x0 = challenge record + 0x18: name hash at -0x10, current step hash at -8, current
        // index at +0x64, step count at +0x68, steps at +0x70 (0x90 bytes each, hash at +0x78),
        // pending step hash at +0x8c.
        constexpr u32 MaxSteps = 256;
        const auto challenge = Read32(memory, x0 - 0x10);
        const auto current = Read32(memory, x0 - 0x8);
        const auto pending = Read32(memory, x0 + 0x8c);
        const auto from_index = Read32(memory, x0 + 0x64);
        const auto count = Read32(memory, x0 + 0x68);
        const auto steps = Read64(memory, x0 + 0x70);
        std::optional<u32> to_index;
        if (pending && count && steps && *count <= MaxSteps) {
            for (u32 i = 0; i < *count; ++i) {
                if (Read32(memory, *steps + u64{i} * 0x90 + 0x78) == pending) {
                    to_index = i;
                }
            }
        }
        const char* direction = "?";
        if (from_index && to_index) {
            direction = *to_index < *from_index ? "backwards"
                                                : (*to_index > *from_index ? "forwards" : "same");
        }
        return fmt::format("gamedata step-apply challenge={} from={} #{} to={} #{} of {} {} "
                           "w1={} lr={}",
                           Hash(challenge), Hash(current), Index(from_index), Hash(pending),
                           Index(to_index), Index(count), direction,
                           static_cast<u32>(jit.GetRegister(1)), caller);
    }
    case Point::PlayedEvent: {
        constexpr u32 MaxName = 128;
        std::string name;
        bool read = false;
        if (const auto task = Read64(memory, x0 + 0x10)) {
            const auto text = Read64(memory, *task + 8);
            const auto length = Read32(memory, *task + 0x10);
            if (text && length && *length >= 1 && *length <= 4096) {
                read = true;
                const u32 size = std::min(*length - 1, MaxName);
                for (u32 i = 0; i < size; ++i) {
                    if (!memory.IsValidVirtualAddressRange(*text + i, 1)) {
                        read = false;
                        break;
                    }
                    const u8 c = memory.Read8(*text + i);
                    if (c == 0) {
                        break;
                    }
                    name.push_back(c >= 0x20 && c < 0x7f ? static_cast<char>(c) : '?');
                }
            }
        }
        if (!read) {
            return fmt::format("gamedata played-event name=? x0={:#x} lr={}", x0, caller);
        }
        return fmt::format("gamedata played-event name={} hash={:#010x} lr={}", name,
                           Common::Nx2EventTrace::Murmur3(name), caller);
    }
    case Point::StructBoolSet:
        return fmt::format("gamedata struct-bool-set member={:08x} value={} mgr={:#x} lr={}",
                           static_cast<u32>(jit.GetRegister(3)),
                           static_cast<u32>(jit.GetRegister(1)), x0, caller);
    case Point::ActorFlagSet:
        return fmt::format("gamedata actor-flag-set member={} key={} value={} actor={:#x} lr={}",
                           Hash(Read32(memory, jit.GetRegister(8) + 8)),
                           fmt::format("{:016x}", Read64(memory, jit.GetRegister(8) + 0x88).value_or(0)),
                           static_cast<u32>(jit.GetRegister(1)), x0, caller);
    case Point::Frame:
        return fmt::format("gamedata frame lr={}", caller);
    case Point::BitRead:
    case Point::BitSet:
        return fmt::format("gamedata {} mgr={:#x} key={:016x}", Name(point), x0,
                           jit.GetRegister(2));
    case Point::None:
        break;
    }
    return {};
}

/// A breakpoint at `pc`: logs and resumes when it is one of the trace's (true), else false. The
/// line goes to EDEN_NX2_GAMEDATA_TRACE's target, or to `out` when one is given (tests).
template <typename Jit, typename Memory>
bool Handle(Session& session, u64 pc, Jit& jit, Memory& memory, std::string* out = nullptr) {
    const auto original = Detail::Read32(memory, pc);
    const Point point = original ? Identify(session.base, pc, *original) : Point::None;
    if (point == Point::None) {
        return false;
    }
    bool last = false;
    if (session.Take(point, last)) {
        auto line = Describe(point, session.base, jit, memory);
        if (last) {
            line += fmt::format(" (the last {} line this session)", Name(point));
        }
        if (out != nullptr) {
            *out = std::move(line);
        } else {
            Common::Nx2EventTrace::Write(Common::Nx2EventTrace::GameDataTarget(), line);
        }
    }
    Resume(point, jit, memory);
    return true;
}

} // namespace Core::OotGameDataTrace
