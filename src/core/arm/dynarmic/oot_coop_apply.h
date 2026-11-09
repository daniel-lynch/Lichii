// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <deque>
#include <optional>

#include "core/arm/dynarmic/oot_coop_sync.h"

namespace Core::OotCoop {

// Backends capture full CPU state and own the private stack and executable sentinel.
struct ApplyPolicy {
    bool enabled{};
    std::set<Kind> classes;
    std::set<std::pair<Kind, u64>> keys;
    u64 wallet{}; // the profile's wallet key; 0 refuses every rupee delta

    bool Allows(const Change& change) const {
        if (change.kind == Kind::Rupee) {
            return enabled && classes.contains(Kind::Rupee) && wallet && change.key == wallet &&
                   RupeeDelta(change.value);
        }
        if (change.kind == Kind::Item) {
            return enabled && classes.contains(Kind::Item) && ValidItem(change);
        }
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
    /// 64-bit stores into the private stack region that Redirect makes before the call (an
    /// argument block the callee reads); empty for the flag setters and the wallet entry.
    std::vector<std::pair<u64, u64>> stores;
    /// The float argument in s0 (bits), for the ghost's alpha and frame setters; unset: v0 as is.
    std::optional<u32> s0_bits;
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
            pending.size() >= QueueLimit) {
            return false;
        }
        // A delta repeats by design (each one is a new origin/sequence); flags commit once.
        if (!EventKind(change.kind)) {
            if (receipts.size() >= StateLimit || receipts.contains(key)) return false;
            receipts.emplace(key, false);
        }
        pending.push_back(change);
        return true;
    }

    bool Suppress(const Change& change) const {
        return change.value == 1 && receipts.contains({change.kind, change.key});
    }

    ApplyPhase Phase() const { return phase; }
    size_t Pending() const { return pending.size(); }
    const std::optional<Change>& LastGrant() const { return last_grant; }
    std::string_view LastPath() const { return last_path; }
    void Invalidate() { Quarantine(); }

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
            // The wallet leaf adds to the pending field synchronously; the game's pouch update
            // applies and clamps it later. The return is the commit.
            // An item give commits the same way: the game's call has run to completion.
            if (EventKind(active->kind)) return Commit();
            if (backend.ReadFlag(*active) == true) return Commit();
            return site.tick >= deadline ? Quarantine() : ApplyEvent::Blocked;
        }
        if (pending.empty()) return ApplyEvent::None;
        active = pending.front();
        last_grant = active;
        last_path = "already-true";
        if (!EventKind(active->kind)) {
            const auto current = backend.ReadFlag(*active);
            if (!current) {
                active.reset();
                return ApplyEvent::Blocked;
            }
            if (*current) return Commit();
        }
        const auto candidate = backend.Resolve(*active);
        if (!candidate || !candidate->verified) {
            active.reset();
            return ApplyEvent::Blocked;
        }
        target = *candidate;
        generation = site.generation;
        thread = site.thread;
        deadline = site.tick > MaxSequence - 120 ? MaxSequence : site.tick + 120;
        last_path = target.pure_flag ? "data-or" : EventKind(active->kind) ? "call" : "setter";
        if (target.pure_flag && EventKind(active->kind)) {
            active.reset();
            return ApplyEvent::Blocked;
        }
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

    // The GameData manager or one of its tables moved (title screen <-> save load). A call in
    // flight would return into the old manager, so that stays a quarantine. A grant awaiting its
    // commit is re-read against the new tables (it is still pending.front()); receipts stay.
    bool Rebind() {
        if (phase == ApplyPhase::Calling || phase == ApplyPhase::Quarantined) return false;
        if (phase == ApplyPhase::AwaitingCommit) {
            active.reset();
            phase = ApplyPhase::Idle;
        }
        return true;
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
        if (!EventKind(active->kind)) receipts.at({active->kind, active->key}) = true;
        pending.pop_front();
        active.reset();
        phase = ApplyPhase::Idle;
        return ApplyEvent::Committed;
    }

    ApplyPolicy policy;
    std::deque<Change> pending;
    std::map<std::pair<Kind, u64>, bool> receipts;
    std::optional<Change> active;
    std::optional<Change> last_grant;
    std::string_view last_path;
    std::optional<typename Backend::Context> saved;
    ApplyTarget target;
    ApplyPhase phase{ApplyPhase::Idle};
    u64 generation{};
    u64 thread{};
    u64 deadline{};
};

} // namespace Core::OotCoop
