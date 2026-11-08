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
};

// Only Bool64 has a proven reader and callable ABI. Its byte is NOT a pure flag store:
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

    bool Bind(u64 manager) {
        using namespace OotGameDataTrace::Detail;
        if (binding.manager && manager != binding.manager) {
            binding.invalid = true;
            return false;
        }
        if (!manager || manager > std::numeric_limits<u64>::max() - 0xC80) return false;
        const auto count = Read32(memory, manager + 0xC58);
        const auto data = Read64(memory, manager + 0xC60);
        const auto queue = Read64(memory, manager + 0xC70);
        if (!count || !data || !queue || !*count || *count > 1'000'000 || !*data ||
            *data > std::numeric_limits<u64>::max() - u64{*count} * 0x10 ||
            !memory.IsValidVirtualAddressRange(*data, u64{*count} * 0x10)) return false;
        if (binding.manager && (binding.manager != manager || binding.count != *count ||
                                binding.data != *data || binding.queue != *queue)) {
            binding.invalid = true;
            return false;
        }
        binding.manager = manager;
        binding.count = *count;
        binding.data = *data;
        binding.queue = *queue;
        return !binding.invalid;
    }

    std::optional<bool> ReadFlag(const Change& change) {
        if (change.kind != Kind::Bit || !change.key || !Bind(binding.manager)) return {};
        u32 low = 0, high = binding.count;
        while (low < high) {
            const u32 mid = low + (high - low) / 2;
            const u64 record = binding.data + u64{mid} * 0x10;
            const u64 key = memory.Read64(record);
            if (key == change.key) {
                const u8 value = memory.Read8(record + 0xC);
                return value <= 1 ? std::optional<bool>{value != 0} : std::nullopt;
            }
            if (key < change.key) low = mid + 1;
            else high = mid;
        }
        return {};
    }

    std::optional<ApplyTarget> Resolve(const Change& change) {
        using namespace OotGameDataTrace::Detail;
        if (!ReadFlag(change).has_value() || !binding.sentinel || binding.stack < 0x4000 ||
            !memory.IsValidVirtualAddressRange(binding.stack - 0x4000, 0x4000) ||
            Read32(memory, binding.base + 0x3761720) != 0xB40003C2 ||
            Read32(memory, binding.base + 0x3761724) != 0xAA0003E8 ||
            Read32(memory, binding.sentinel) != OotGameDataTrace::Breakpoint) return {};
        const auto capacity = Read32(memory, binding.manager + 0xC68);
        const auto state = Read32(memory, binding.manager + 0xC78);
        if (!capacity || !state || !*capacity || *capacity > 0xFFFFF ||
            (*state & 0xFFFFF) >= *capacity || !binding.queue ||
            !memory.IsValidVirtualAddressRange(binding.queue, u64{*capacity} * 8)) return {};
        return ApplyTarget{true, false, 0, 0, binding.base + 0x3761720, binding.sentinel,
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
    bool baseline{}, logged_quarantine{};
};
} // namespace Core::OotCoop
