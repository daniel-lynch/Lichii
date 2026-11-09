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
#include <utility>
#include <vector>

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
/// The sites belong to one build of the game's main module. Their module offsets, the build id
/// and every other game-derived constant come from a user-supplied private profile (the
/// "offsets" object of the EDEN_NX2_OOT_COOP_KEYS file, keyed by build id); none is compiled in.
/// What each point's replaced instruction is, and so what Resume does, is fixed here:
/// - StepSet: the quest step setter, hooked at its `mov w8, #1` (x0-x5 are its arguments).
/// - StepApply / PlayedEvent / Frame / StructBoolSet / ActorFlagSet: a `pacibsp` (a no-op).
/// - BitRead: `ldr w8, [x0, #0xc58]`; BitSet: `mov x8, x0`.
/// - ItemGive: the `pacibsp` at the entry of the game's give-to-pouch call (co-op item class).
namespace Core::OotGameDataTrace {

inline constexpr u64 ProgramId = 0x0400486024DEC000ULL;

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
    ItemGive,
};
inline constexpr size_t PointCount = 10;

struct Site {
    Point point;
    u64 offset;      ///< From the main module's base
    u32 instruction; ///< The original instruction there; any other word means no hook
};

inline constexpr u32 MovW8One = 0x52800028; // mov w8, #1
inline constexpr u32 Pacibsp = 0xD503237F;  // pacibsp (a hint: dynarmic runs it as a no-op)
inline constexpr u32 Breakpoint = 0xD4200000; // brk #0
inline constexpr u32 LdrW8C58 = 0xB94C5808; // ldr w8, [x0, #0xc58]
inline constexpr u32 MovX8X0 = 0xAA0003E8;  // mov x8, x0

/// The only instruction Resume can stand in for at `point`.
constexpr u32 ReplacedInstruction(Point point) {
    switch (point) {
    case Point::StepSet: return MovW8One;
    case Point::BitRead: return LdrW8C58;
    case Point::BitSet: return MovX8X0;
    case Point::None: return 0;
    default: return Pacibsp;
    }
}

/// A guest function the co-op drain may call: its module offset and the first words it must have.
struct Entry {
    u64 offset{};
    std::vector<u32> words;
    explicit operator bool() const { return offset != 0 && !words.empty(); }
};

/// The ghost ("pose" class): the reads that make a pose and the calls that draw a peer's puppet.
struct GhostOffsets {
    u64 playing_info_cell{}; ///< Module offset of the player-info pointer (pose source)
    u64 pos{}, rot{};        ///< Fields of that object: position (3 floats), rotation 3x3 (9 floats)
    u64 actor_mgr_cell{};    ///< Module offset of the actor manager pointer
    u64 scale_const{};       ///< Module offset of the 12-byte unit scale the create argument copies
    u32 banc_hash{}, child_hash{}, loading_hash{}; ///< GameData keys: String64, Bool, Bool
    std::array<u64, 3> str64{}; ///< String64 table: {table offset in the manager, stride, char* field}
    Entry create, get, release, set_mtx;
    std::vector<u64> actor_fields; ///< Actor pointer fields that must stay unchanged and non-null
    /// The game's own actor-link check, as pure reads: {actor's lifetime record field, actor's
    /// id field, record's actor field, record's generation field}. Live while the record still
    /// names this actor with the generation the actor had when it was taken.
    std::array<u64, 4> link{};
    std::string actor_child, actor_adult; ///< Actor names; adult falls back to child
    bool Valid() const {
        return playing_info_cell && pos && rot && actor_mgr_cell && scale_const && banc_hash &&
               child_hash && loading_hash && str64[0] && str64[1] && str64[2] && create && get &&
               release && set_mtx && !actor_fields.empty() && link[0] && link[1] && link[2] && link[3] &&
               !actor_child.empty();
    }
};

/// Everything game-derived for one main build, loaded at run time from the private profile.
struct Profile {
    std::string build_id;
    std::vector<Site> sites;
    u64 manager_cell{}; ///< Module offset of the GameData manager pointer
    u64 pouch_cell{};   ///< Module offset of the pouch manager pointer
    u32 wallet_hash{};  ///< GameData key of the wallet (rupee deltas)
    Entry bool_setter, bit_setter, rupee_entry;
    Entry item_entry;               ///< The give-to-pouch call (the ItemGive site's function)
    std::vector<u64> item_callers;  ///< Return addresses (module offsets) of gives to forward
    u64 event_got{};                ///< Module offset of the GOT slot of the event manager's cell
    /// {cell, field}: *(*(main + cell) + field) must be non-zero before a give is replayed.
    std::vector<std::pair<u64, u64>> item_guards;
    GhostOffsets ghost; ///< Empty unless the profile has a "ghost" object

    u64 SiteOffset(Point point) const {
        for (const Site& site : sites) {
            if (site.point == point) return site.offset;
        }
        return 0;
    }
};

/// Co-op alone hooks only the frame boundary: it binds GameData from the manager cell there, and
/// its handler ignores every other point. The Bool64 accessors are hot (the actor reader copy
/// runs per actor per frame), so hooking them without tracing would cost a trap each.
constexpr bool CoopPoint(Point point) {
    return point == Point::Frame || point == Point::ItemGive;
}

/// The trace point at `address` whose original instruction is `instruction`, or None.
inline Point Identify(const Profile& profile, u64 base, u64 address, u32 instruction) {
    if (base == 0 || address < base) {
        return Point::None;
    }
    for (const Site& site : profile.sites) {
        if (address - base == site.offset && instruction == site.instruction) {
            return site.point;
        }
    }
    return Point::None;
}

/// What the JIT translates at `address`: a breakpoint at a trace point, the original otherwise.
inline u32 InstructionFor(const Profile& profile, u64 base, u64 address, u32 instruction) {
    return Identify(profile, base, address, instruction) == Point::None ? instruction : Breakpoint;
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
    case Point::ItemGive:
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
    case Point::ItemGive: return "item-give";
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

    Session(const Kernel::KProcess* owner_, u64 base_, u64 id_,
            std::shared_ptr<const Profile> profile_)
        : owner{owner_}, base{base_}, id{id_}, profile{std::move(profile_)} {}

    /// Whether this hit may log: true LineLimit times per point. `last` is set on the last one.
    bool Take(Point point, bool& last) {
        const u32 count = lines[static_cast<size_t>(point)].fetch_add(1, std::memory_order_relaxed);
        last = count + 1 == LineLimit;
        return count < LineLimit;
    }

    const Kernel::KProcess* const owner;
    const u64 base;
    const u64 id;
    const std::shared_ptr<const Profile> profile; ///< Never null

private:
    std::array<std::atomic<u32>, PointCount> lines{};
};

class Sessions {
public:
    /// The main module of `owner` loaded at `base` (0 or no profile: not a profiled build, no hooks).
    void Reset(const Kernel::KProcess* owner, u64 base,
               std::shared_ptr<const Profile> profile = nullptr) {
        auto session = base != 0 && profile
                           ? std::make_shared<Session>(owner, base, next_id.fetch_add(1),
                                                       std::move(profile))
                           : nullptr;
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
/// current step hash at +0x10 (the step setter's own search).
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
    case Point::ItemGive:
        return fmt::format("gamedata item-give category={} count={} get={} lr={}",
                           static_cast<u32>(jit.GetRegister(2)), static_cast<u32>(jit.GetRegister(3)),
                           jit.GetRegister(4), caller);
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
    const Point point =
        original ? Identify(*session.profile, session.base, pc, *original) : Point::None;
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
