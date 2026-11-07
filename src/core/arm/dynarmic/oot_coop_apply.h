// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <deque>

#include "core/arm/dynarmic/oot_coop_sync.h"

namespace Core::OotCoop {

// This engine has no production memory/JIT adapter. A verified adapter must capture all CPU
// state, own a private stack and executable sentinel, and validate the current save generation.
struct ApplyPolicy {
    bool enabled{};
    std::set<Kind> classes;
    std::set<std::pair<Kind, u64>> keys;

    bool Allows(const Change& change) const {
        return enabled && WireKind(change.kind) && change.value == 1 &&
               classes.contains(change.kind) && keys.contains({change.kind, change.key});
    }
};

struct DrainContext {
    u64 thread{};
    u64 generation{};
    u64 tick{};
    bool safe{}; // Verified frame boundary, save loaded, no transition or active event.
};

struct ApplyTarget {
    bool verified{};
    bool pure_flag{};
    u64 address{};
    u64 mask{};
    u64 entry{};
    u64 sentinel{};
    u64 stack{};
    std::array<u64, 8> arguments{};
};

enum class ApplyPhase { Idle, Calling, AwaitingCommit, Quarantined };
enum class ApplyEvent { None, Blocked, Started, Returned, Committed, Recovered, Quarantined };

// Backend operations return failure instead of throwing. Context must include GPRs, SIMD,
// PC/SP/PSTATE/FPCR/FPSR, TLS and exclusive-monitor handling. Restore must be all-or-nothing.
// Suppression belongs to the save generation, not a host thread or a C++ scope.
template <typename Backend>
class GrantApplier {
public:
    explicit GrantApplier(ApplyPolicy policy_) : policy{std::move(policy_)} {}

    bool Queue(const Change& change) {
        const auto key = std::pair{change.kind, change.key};
        if (phase == ApplyPhase::Quarantined || !policy.Allows(change) ||
            pending.size() >= QueueLimit || receipts.size() >= StateLimit || receipts.contains(key)) {
            return false;
        }
        receipts.emplace(key, false);
        pending.push_back(change);
        return true;
    }

    bool Suppress(const Change& change) const {
        return change.value == 1 && receipts.contains({change.kind, change.key});
    }

    ApplyPhase Phase() const { return phase; }
    size_t Pending() const { return pending.size(); }

    ApplyEvent Drain(Backend& backend, const DrainContext& site) {
        if (phase == ApplyPhase::Quarantined) return ApplyEvent::Quarantined;
        if (phase == ApplyPhase::Calling) return ApplyEvent::Blocked;
        if (!site.safe || !policy.enabled) return ApplyEvent::Blocked;
        if (!site.generation || !site.thread) return ApplyEvent::Blocked;
        if (generation && (site.generation != generation || site.thread != thread)) {
            return Quarantine();
        }
        if (!generation && !pending.empty()) {
            generation = site.generation;
            thread = site.thread;
        }
        if (phase == ApplyPhase::AwaitingCommit) {
            if (backend.ReadFlag(*active) == true) return Commit();
            return site.tick >= deadline ? Quarantine() : ApplyEvent::Blocked;
        }
        if (pending.empty()) return ApplyEvent::None;
        active = pending.front();
        const auto current = backend.ReadFlag(*active);
        if (!current) {
            active.reset();
            return ApplyEvent::Blocked;
        }
        if (*current) return Commit();
        const auto candidate = backend.Resolve(*active);
        if (!candidate || !candidate->verified) {
            active.reset();
            return ApplyEvent::Blocked;
        }
        target = *candidate;
        generation = site.generation;
        thread = site.thread;
        deadline = site.tick > MaxSequence - 120 ? MaxSequence : site.tick + 120;
        if (target.pure_flag) {
            // The adapter must prove this is the authoritative word with no notifications,
            // queue, dirty bits or concurrent writer; OrFlag is an atomic masked update.
            if (!target.address || !target.mask || !backend.OrFlag(target)) return Quarantine();
            phase = ApplyPhase::AwaitingCommit;
            return backend.ReadFlag(*active) == true ? Commit() : ApplyEvent::Started;
        }
        if (!target.entry || !target.sentinel || target.entry == target.sentinel ||
            (target.entry & 3) || (target.sentinel & 3) || !target.stack || (target.stack & 15)) {
            active.reset();
            return ApplyEvent::Blocked;
        }
        saved = backend.Capture();
        if (!saved) {
            active.reset();
            return ApplyEvent::Blocked;
        }
        // Redirect must fail without changing CPU or memory state.
        if (!backend.Redirect(target)) {
            saved.reset();
            active.reset();
            return ApplyEvent::Blocked;
        }
        phase = ApplyPhase::Calling;
        return ApplyEvent::Started;
    }

    ApplyEvent Sentinel(Backend& backend, const DrainContext& site, u64 pc) {
        if (phase != ApplyPhase::Calling || pc != target.sentinel || site.thread != thread) {
            return ApplyEvent::Blocked;
        }
        if (site.generation != generation || !backend.AtReturnStack(target) ||
            !backend.Restore(*saved)) return Quarantine();
        saved.reset();
        phase = ApplyPhase::AwaitingCommit;
        // A setter return only acknowledges enqueueing. Observe the authoritative value later.
        return ApplyEvent::Returned;
    }

    ApplyEvent Recover(Backend& backend, const DrainContext& site) {
        if (phase != ApplyPhase::Calling) return ApplyEvent::Blocked;
        if (site.thread != thread || site.generation != generation ||
            !backend.CanRestore() || !backend.Restore(*saved)) return Quarantine();
        saved.reset();
        phase = ApplyPhase::Quarantined;
        return ApplyEvent::Recovered;
    }

    ApplyEvent Watchdog(Backend& backend, const DrainContext& site) {
        if (phase == ApplyPhase::Calling && site.tick >= deadline) return Recover(backend, site);
        return ApplyEvent::None;
    }

    // Called only after the old guest thread/process is destroyed, never while it can return.
    void ResetAfterGuestStopped() {
        pending.clear();
        receipts.clear();
        saved.reset();
        active.reset();
        phase = ApplyPhase::Idle;
        generation = thread = deadline = 0;
    }

private:
    ApplyEvent Quarantine() {
        phase = ApplyPhase::Quarantined;
        return ApplyEvent::Quarantined;
    }
    ApplyEvent Commit() {
        receipts.at({active->kind, active->key}) = true;
        pending.pop_front();
        active.reset();
        phase = ApplyPhase::Idle;
        return ApplyEvent::Committed;
    }

    ApplyPolicy policy;
    std::deque<Change> pending;
    std::map<std::pair<Kind, u64>, bool> receipts;
    std::optional<Change> active;
    std::optional<typename Backend::Context> saved;
    ApplyTarget target;
    ApplyPhase phase{ApplyPhase::Idle};
    u64 generation{};
    u64 thread{};
    u64 deadline{};
};

} // namespace Core::OotCoop
