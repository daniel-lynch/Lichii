// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <limits>
#include "core/arm/dynarmic/oot_coop_apply.h"

namespace Core::OotCoop {

inline std::string_view ClassName(Kind kind) {
    return kind == Kind::Bit ? "bit" : "bool";
}

struct GuestBinding {
    u64 base{}, manager{}, data{}, queue{}, sentinel{}, stack{};
    u32 count{};
    bool invalid{};
    u64 bool_data{}, bool_queue{}, bool_hashes{};
    u32 bool_count{}, bool_capacity{};
};

inline constexpr std::array<std::pair<u32, u32>, 206> ChestMembers{{
#include "core/arm/dynarmic/oot_coop_chest_keys.inc"
}};

inline std::optional<u32> ChestBool(u32 member) {
    const auto it = std::lower_bound(ChestMembers.begin(), ChestMembers.end(), member,
                                    [](const auto& row, u32 hash) { return row.first < hash; });
    return it != ChestMembers.end() && it->first == member ? std::optional{it->second} : std::nullopt;
}

inline bool IsChestBool(u64 key) {
    return std::any_of(ChestMembers.begin(), ChestMembers.end(),
                       [key](const auto& row) { return row.second == key; });
}

// Both flag classes use deferred guest setters, never direct byte stores:
// the game's queue consumer owns notifications and dirty tracking (see HOOKS.md).
template <typename Jit, typename Memory>
class GameDataBackend {
public:
    struct Context {
        std::array<u64, 31> x;
        std::array<std::array<u64, 2>, 32> v;
        u64 pc, sp, tls, tls_ro;
        u32 pstate, fpcr, fpsr;
        bool operator==(const Context&) const = default;
    };

    GameDataBackend(Jit& jit_, Memory& memory_, GuestBinding& binding_, u64& tls_, u64& tls_ro_,
                    std::function<void()> clear_exclusive_)
        : jit{jit_}, memory{memory_}, binding{binding_}, tls{tls_}, tls_ro{tls_ro_},
          clear_exclusive{std::move(clear_exclusive_)} {}

    /// Did the manager cell or a bound table change since binding? Unreadable counts as moved.
    bool Moved() {
        using namespace OotGameDataTrace::Detail;
        if (!binding.manager) return false;
        const u64 m = binding.manager;
        if (Read64(memory, binding.base + 0x4E842A8) != m) return true;
        if (binding.bool_data &&
            (Read32(memory, m + 0x10) != binding.bool_count ||
             Read64(memory, m + 0x18) != binding.bool_data ||
             Read64(memory, m + 0x38) != binding.bool_queue ||
             Read64(memory, m + 0x20) != binding.bool_hashes ||
             Read32(memory, m + 0x2C) != binding.bool_capacity)) return true;
        return binding.data && (Read32(memory, m + 0xC58) != binding.count ||
                                Read64(memory, m + 0xC60) != binding.data ||
                                Read64(memory, m + 0xC70) != binding.queue);
    }

    /// Forget the manager and table identities; the private stack and sentinel stay owned.
    void Unbind() {
        binding = GuestBinding{binding.base, 0, 0, 0, binding.sentinel, binding.stack};
    }

    bool BindFromCell() {
        const auto manager = OotGameDataTrace::Detail::Read64(memory, binding.base + 0x4E842A8);
        return BindManager(manager.value_or(0));
    }

    bool BindManager(u64 manager) {
        if (binding.manager && manager != binding.manager) binding.invalid = true;
        if (binding.invalid || !manager || manager > std::numeric_limits<u64>::max() - 0xC80)
            return false;
        binding.manager = manager;
        return true;
    }

    bool Bind(u64 manager, Kind kind = Kind::Bit) {
        using namespace OotGameDataTrace::Detail;
        if (!BindManager(manager)) return false;
        const bool flat = kind == Kind::Bool;
        if (!flat && kind != Kind::Bit) return false;
        const auto count = Read32(memory, manager + (flat ? 0x10 : 0xC58));
        const auto data = Read64(memory, manager + (flat ? 0x18 : 0xC60));
        const auto queue = Read64(memory, manager + (flat ? 0x38 : 0xC70));
        const u64 stride = flat ? 0x18 : 0x10;
        if (!count || !data || !queue || !*count || *count > 1'000'000 ||
            !Span(*data, u64{*count} * stride) ||
            (!flat && !Range(*data, u64{*count} * stride))) return false;
        auto& old_count = flat ? binding.bool_count : binding.count;
        auto& old_data = flat ? binding.bool_data : binding.data;
        auto& old_queue = flat ? binding.bool_queue : binding.queue;
        if (old_data && (old_count != *count || old_data != *data || old_queue != *queue)) {
            binding.invalid = true;
            return false;
        }
        if (flat) {
            const auto hashes = Read64(memory, manager + 0x20);
            const auto capacity = Read32(memory, manager + 0x2C);
            if (!hashes || !capacity || !*capacity || *capacity > 2'000'000 ||
                !Span(*hashes, u64{*capacity} * 8)) return false;
            if (binding.bool_hashes && (binding.bool_hashes != *hashes ||
                                        binding.bool_capacity != *capacity)) {
                binding.invalid = true;
                return false;
            }
            binding.bool_hashes = *hashes;
            binding.bool_capacity = *capacity;
        }
        old_count = *count;
        old_data = *data;
        old_queue = *queue;
        return true;
    }

    std::optional<bool> ReadFlag(const Change& change) {
        if (!change.key || (change.kind == Kind::Bool && !IsChestBool(change.key)) ||
            !Bind(binding.manager, change.kind)) return {};
        if (change.kind == Kind::Bool) {
            // Same modulo/linear probe as the guest. Bound corrupt/collision-heavy tables.
            u32 slot = static_cast<u32>(change.key % binding.bool_capacity);
            for (u32 probe = 0; probe < std::min(binding.bool_capacity, 64U); ++probe) {
                const u64 address = binding.bool_hashes + u64{slot} * 8;
                if (!Range(address, 8)) return {};
                const u32 key = memory.Read32(address);
                if (!key) return {};
                if (key == change.key) {
                    const u32 index = memory.Read32(address + 4);
                    if (index >= binding.bool_count) return {};
                    return Value(binding.bool_data + u64{index} * 0x18 + 0x16);
                }
                slot = (slot + 1) % binding.bool_capacity;
            }
            return {};
        }
        u32 low = 0, high = binding.count;
        while (low < high) {
            const u32 mid = low + (high - low) / 2;
            const u64 record = binding.data + u64{mid} * 0x10;
            const u64 key = memory.Read64(record);
            if (key == change.key) return Value(record + 0xC);
            if (key < change.key) low = mid + 1;
            else high = mid;
        }
        return {};
    }

    std::optional<ApplyTarget> Resolve(const Change& change) {
        using namespace OotGameDataTrace::Detail;
        if (!ReadFlag(change).has_value() || !binding.sentinel || binding.stack < 0x4000 ||
            !Range(binding.stack - 0x4000, 0x4000) ||
            Read32(memory, binding.sentinel) != OotGameDataTrace::Breakpoint) return {};
        const bool flat = change.kind == Kind::Bool;
        const u64 entry = binding.base + (flat ? 0x682B20 : 0x3761720);
        if (flat) {
            constexpr std::array<u32, 4> words{0xD503237F, 0xD100C3FF, 0xA9017BFD, 0xF90013F3};
            for (size_t i = 0; i < words.size(); ++i)
                if (Read32(memory, entry + i * 4) != words[i]) return {};
        } else if (Read32(memory, entry) != 0xB40003C2 ||
                   Read32(memory, entry + 4) != 0xAA0003E8) return {};
        const auto capacity = Read32(memory, binding.manager + (flat ? 0x30 : 0xC68));
        const auto state = Read32(memory, binding.manager + (flat ? 0x40 : 0xC78));
        const u64 queue = flat ? binding.bool_queue : binding.queue;
        if (!capacity || !state || !*capacity || *capacity > 0xFFFFF ||
            (*state & 0xFFFFF) >= *capacity || !Range(queue, u64{*capacity} * 8)) return {};
        return ApplyTarget{true, false, 0, 0, entry, binding.sentinel,
                           binding.stack, {binding.manager, 1, change.key}};
    }

    bool OrFlag(const ApplyTarget&) { return false; }
    std::optional<Context> Capture() {
        return Context{jit.GetRegisters(), jit.GetVectors(), jit.GetPC(), jit.GetSP(), tls, tls_ro,
                       jit.GetPstate(), jit.GetFpcr(), jit.GetFpsr()};
    }
    bool Redirect(const ApplyTarget& target) {
        if (binding.invalid || target.stack != binding.stack || target.sentinel != binding.sentinel)
            return false;
        // Reservations are discarded, as on a scheduler context switch: STXR must retry.
        jit.ClearExclusiveState();
        clear_exclusive();
        for (size_t i = 0; i < target.arguments.size(); ++i) jit.SetRegister(i, target.arguments[i]);
        jit.SetRegister(30, target.sentinel);
        jit.SetSP(target.stack);
        jit.SetPC(target.entry);
        return true;
    }
    bool AtReturnStack(const ApplyTarget& target) { return jit.GetSP() == target.stack; }
    bool CanRestore() { return false; } // Never unwind an arbitrary setter/lock site.
    bool Restore(const Context& saved) {
        jit.SetRegisters(saved.x);
        jit.SetVectors(saved.v);
        jit.SetSP(saved.sp);
        jit.SetPC(saved.pc);
        jit.SetPstate(saved.pstate);
        jit.SetFpcr(saved.fpcr);
        jit.SetFpsr(saved.fpsr);
        tls = saved.tls;
        tls_ro = saved.tls_ro;
        jit.ClearExclusiveState();
        clear_exclusive();
        return true;
    }

private:
    static bool Span(u64 address, u64 size) {
        return address && address <= std::numeric_limits<u64>::max() - size;
    }
    bool Range(u64 address, u64 size) {
        return Span(address, size) && memory.IsValidVirtualAddressRange(address, size);
    }
    std::optional<bool> Value(u64 address) {
        if (!Range(address, 1)) return {};
        const u8 value = memory.Read8(address);
        return value <= 1 ? std::optional<bool>{value != 0} : std::nullopt;
    }
    Jit& jit;
    Memory& memory;
    GuestBinding& binding;
    u64& tls;
    u64& tls_ro;
    std::function<void()> clear_exclusive;
};

// One instance per process/save epoch, shared across host workers. Only the owning guest
// frame thread drains. Network queues are detached before entering this code.
template <typename Backend>
class FrameDrain {
public:
    explicit FrameDrain(ApplyPolicy policy_) : policy{std::move(policy_)}, applier{policy} {}

    bool Queue(const Change& change) { return applier.Queue(change); }
    bool Suppress(const Change& change) const { return applier.Suppress(change); }
    bool Calling() const { return applier.Phase() == ApplyPhase::Calling; }
    bool BaselineReady() const { return baseline; }
    void Invalidate() { applier.Invalidate(); }

    /// The manager moved or went away (title screen <-> save load): forget the old epoch's
    /// baseline instead of reading the new tables as a rollback, then bind afresh. Fails, and
    /// quarantines, only while a synthetic call could still return into the old manager.
    template <typename Log>
    bool Rebind(Backend& backend, Log log) {
        if (!applier.Rebind()) {
            Invalidate();
            Report(ApplyEvent::Quarantined, log);
            return false;
        }
        observed.clear();
        baseline = false;
        logged_waiting = false;
        backend.Unbind();
        return true;
    }

    template <typename Send, typename Log>
    ApplyEvent Frame(Backend& backend, const DrainContext& site, Send send, Log log) {
        if (!site.safe || !site.thread || !site.generation) return ApplyEvent::Blocked;
        if (!thread) { thread = site.thread; generation = site.generation; }
        if (thread != site.thread) return ApplyEvent::Blocked;
        if (generation != site.generation) Invalidate();
        if (applier.Phase() == ApplyPhase::Quarantined) return Report(ApplyEvent::Quarantined, log);
        if (Calling()) return Report(applier.Watchdog(backend, site), log);
        // Read the complete reviewed set before publishing any baseline or changing the guest.
        std::vector<std::pair<Change, bool>> values;
        for (const auto& [kind, key] : policy.keys) {
            if (!policy.classes.contains(kind)) continue;
            Change change{kind, {}, 0, key, 1};
            const auto value = backend.ReadFlag(change);
            if (!value) {
                if (!baseline && !logged_waiting) {
                    log(fmt::format("OoT co-op: baseline waiting: class={} key={:016x} unreadable",
                                    ClassName(kind), key));
                    logged_waiting = true;
                }
                if (baseline) Invalidate();
                return Report(baseline ? ApplyEvent::Quarantined : ApplyEvent::Blocked, log);
            }
            const auto old = observed.find({kind, key});
            if (old != observed.end() && old->second && !*value) {
                Invalidate();
                return Report(ApplyEvent::Quarantined, log);
            }
            values.emplace_back(change, *value);
        }
        for (const auto& [change, value] : values) {
            const auto key = std::pair{change.kind, change.key};
            if (value && (!observed.contains(key) || !observed.at(key))) {
                if (Suppress(change)) log(Line("suppressed", change, "observer"));
                else send(change, !baseline);
            }
            observed[key] = value;
        }
        baseline = true;
        return Report(applier.Drain(backend, site), log);
    }

    template <typename Log>
    ApplyEvent Sentinel(Backend& backend, const DrainContext& site, u64 pc, Log log) {
        return Report(applier.Sentinel(backend, site, pc), log);
    }

private:
    std::string Line(std::string_view event, const Change& change, std::string_view path) const {
        return fmt::format("OoT co-op: {} class={} key={:016x} value={} path={} echo={}-{}-{:016x}",
                           event, ClassName(change.kind), change.key, change.value, path,
                           generation, ClassName(change.kind), change.key);
    }
    template <typename Log>
    ApplyEvent Report(ApplyEvent event, Log log) {
        if (event == ApplyEvent::Quarantined) {
            if (!logged_quarantine) log("OoT co-op: quarantined; restart disposable session");
            logged_quarantine = true;
        } else if (applier.LastGrant() && (event == ApplyEvent::Started ||
                   event == ApplyEvent::Returned || event == ApplyEvent::Committed)) {
            log(Line(event == ApplyEvent::Committed ? "applied" :
                     event == ApplyEvent::Returned ? "returned" : "started",
                     *applier.LastGrant(), applier.LastPath()));
        }
        return event;
    }
    ApplyPolicy policy;
    GrantApplier<Backend> applier;
    std::map<std::pair<Kind, u64>, bool> observed;
    u64 thread{}, generation{};
    bool baseline{}, logged_quarantine{}, logged_waiting{};
};
} // namespace Core::OotCoop
