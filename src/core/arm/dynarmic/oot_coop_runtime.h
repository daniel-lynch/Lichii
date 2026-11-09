// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <functional>
#include <string>
#include <limits>
#include "core/arm/dynarmic/oot_coop_apply.h"

namespace Core::OotCoop {

inline std::string_view ClassName(Kind kind) {
    return kind == Kind::Bit ? "bit" : kind == Kind::Rupee ? "rupee" : kind == Kind::Item ? "item"
           : kind == Kind::Pose ? "pose" : kind == Kind::Key ? "key" : "bool";
}

// The pouch manager is *(main + profile.pouch_cell). The wallet entry (profile.rupee_entry; x0 =
// pouchMgr, w1 = signed delta, w2 = reason) adds into the pending field at pouchMgr+0x3BD4 (reason
// at +0x3BD8); the game's own pouch update later clamps it into the wallet.
inline constexpr u64 RupeeReason = 7; // the one-shot "increase rupee" caller's reason

// An item give replays the game's give-to-pouch call (profile.item_entry): x0 = pouchMgr, x1 = &cell
// where cell = {const char* name}, w2 = category, w3 = count, x4 = get type, w5..w7 = flags, and a
// ninth argument byte at [sp]. The call gets a private frame ItemFrame bytes below the top of the
// private stack: [sp, sp+16) the stack argument, cell at sp+0x20, the name at sp+0x30 (NUL-padded),
// so the callee's own frames grow down from sp into the rest of the stack.
inline constexpr u64 ItemFrame = 0x100;
inline constexpr u64 ItemStackNeed = 0x10000; // mapped bytes below the top before a give is tried

struct GuestBinding {
    u64 base{}, manager{}, data{}, queue{}, sentinel{}, stack{};
    u32 count{};
    bool invalid{};
    u64 bool_data{}, bool_queue{}, bool_hashes{};
    u32 bool_count{}, bool_capacity{};
};

/// What the opener's game passed to its give-to-pouch call, read at the trapped entry.
struct ItemCall {
    Change change;
    bool forward{}; ///< A give from a reviewed caller (a chest's event flow), well formed
    bool pickup{};  ///< From a world-pickup caller (forwarded only for the profile's names)
    bool replay{};  ///< Our own synthetic call (returns to the sentinel): never forwarded
    bool nested{};  ///< Called from inside the give itself (sub-items): the replay recurses too
    std::string why; ///< Why a top-level give was not forwarded ("per-player name", ...)
    std::string line;
};

template <typename Jit, typename Memory>
ItemCall ReadItemCall(Jit& jit, Memory& memory, u64 base, const OotGameDataTrace::Profile& profile,
                      u64 sentinel, bool item_class) {
    using OotGameDataTrace::Detail::Read32;
    using OotGameDataTrace::Detail::Read64;
    ItemCall call;
    const u64 lr = jit.GetRegister(30);
    const u64 caller = lr >= base ? lr - base : 0;
    std::string name; // x1 points at a cell holding a NUL-terminated char*
    const auto text = Read64(memory, jit.GetRegister(1));
    if (text && *text) {
        for (u64 i = 0; i <= ItemNameLimit; ++i) {
            if (!memory.IsValidVirtualAddressRange(*text + i, 1)) { name.clear(); break; }
            const u8 c = memory.Read8(*text + i);
            if (c == 0) break;
            name.push_back(static_cast<char>(c));
        }
    }
    const auto stack = Read32(memory, jit.GetSP());
    const u32 w2 = static_cast<u32>(jit.GetRegister(2)), w5 = static_cast<u32>(jit.GetRegister(5));
    const u32 w6 = static_cast<u32>(jit.GetRegister(6)), w7 = static_cast<u32>(jit.GetRegister(7));
    const u64 x4 = jit.GetRegister(4);
    const ItemArgs args{w2 & 0xFF, static_cast<u32>(x4 & 0xFF),
                        {static_cast<u8>(w5), static_cast<u8>(w6), static_cast<u8>(w7),
                         static_cast<u8>(stack.value_or(0) & 0xFF)}};
    call.change = Change{Kind::Item, {}, 0, PackItem(args),
                         static_cast<s64>(static_cast<s32>(jit.GetRegister(3)))};
    call.change.name = name;
    call.replay = sentinel && lr == sentinel;
    const auto listed = [](const auto& list, const auto& value) {
        return std::find(list.begin(), list.end(), value) != list.end();
    };
    const bool reviewed = listed(profile.item_callers, caller);
    // World pickups: the collect path's give. Allow-list mode (no "give" object): forwarded only
    // for the names that are shared (rupees, permanent items).
    call.pickup = listed(profile.pickup_callers, caller);
    const bool shared_pickup = call.pickup && listed(profile.pickup_names, name);
    call.nested = !call.replay && caller >= profile.give_body[0] && caller < profile.give_body[1];
    // Only calls whose small arguments survive the packing are forwarded.
    const bool valid = ValidItem(call.change) && w2 < 16 && x4 < 16 && w5 <= 0xFF && w6 <= 0xFF &&
                       w7 <= 0xFF && stack.has_value();
    bool wanted = reviewed || shared_pickup;
    if (profile.give_all) {
        // Generic mode (Anchor's one hook): every top-level give, minus the explicit deny lists.
        call.why = call.nested                                   ? "nested"
                   : listed(profile.give_deny_callers, caller)   ? "per-player caller"
                   : listed(profile.give_never, name)            ? "never shared"
                   : call.pickup && listed(profile.give_per_player, name) ? "per-player name"
                                                                 : "";
        wanted = call.why.empty();
    } else if (call.pickup && !shared_pickup) {
        call.why = "per-player";
    } else if (call.pickup && !item_class) {
        call.why = "not forwarded";
    } else if (!wanted) {
        call.why = "unreviewed caller";
    }
    if (!valid && !call.replay && !call.nested && call.why.empty()) call.why = "malformed";
    call.forward = item_class && !call.replay && !call.nested && wanted && valid;
    const std::string_view tag = call.replay ? "" : call.nested ? " (nested)"
                                 : call.pickup ? (call.forward ? " (pickup)" : "") : "";
    call.line = fmt::format("item-give {}{}{} name={} category={} count={} get={} flags={},{},{},{} lr={}",
                            call.replay ? "replayed" : call.forward ? "observed" : "seen", tag,
                            call.forward || call.replay || call.nested ? std::string{}
                            : call.why.empty() ? std::string{" (class off)"}
                            : fmt::format(" ({}{})", call.pickup ? "pickup, " : "", call.why),
                            name.empty() ? "?" : name, w2, static_cast<s32>(jit.GetRegister(3)), x4,
                            w5, w6, w7, args.flags[3],
                            call.replay ? std::string{"sentinel"} : fmt::format("main+{:#x}", caller));
    return call;
}

/// A wallet-entry call (x0 = pouchMgr, w1 = signed delta, w2 = reason) read at its first
/// instruction: signed deltas from reviewed gameplay callers, excluding every applied call.
struct RupeeCall {
    Change change;
    bool forward{};
    bool replay{};
    std::string line;
};

template <typename Jit>
RupeeCall ReadRupeeCall(Jit& jit, u64 base, const OotGameDataTrace::Profile& profile, u64 sentinel,
                        bool rupee_class, bool applying = false) {
    RupeeCall call;
    const u64 lr = jit.GetRegister(30);
    const u64 caller = lr >= base ? lr - base : 0;
    const s64 delta = static_cast<s32>(jit.GetRegister(1));
    call.replay = applying || (sentinel && lr == sentinel);
    call.change = Change{Kind::Rupee, {}, 0, profile.wallet_hash, delta};
    const bool listed = std::find(profile.rupee_callers.begin(), profile.rupee_callers.end(),
                                  caller) != profile.rupee_callers.end() ||
                        (profile.world.shop_lr && caller == profile.world.shop_lr);
    call.forward = rupee_class && !call.replay && listed && profile.wallet_hash &&
                   RupeeDelta(delta);
    call.line = fmt::format("rupee-add {} delta={} reason={} lr={}",
                            call.replay ? "replayed" : call.forward ? "observed"
                            : !listed ? "seen (unlisted caller)" : !RupeeDelta(delta) ? "seen (invalid delta)"
                                                                 : "seen (class off)",
                            delta, static_cast<u32>(jit.GetRegister(2)),
                            call.replay ? std::string{"sentinel"} : fmt::format("main+{:#x}", caller));
    return call;
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

    GameDataBackend(Jit& jit_, Memory& memory_, GuestBinding& binding_,
                    const OotGameDataTrace::Profile& profile_, u64& tls_, u64& tls_ro_,
                    std::function<void()> clear_exclusive_)
        : jit{jit_}, memory{memory_}, binding{binding_}, profile{profile_}, tls{tls_}, tls_ro{tls_ro_},
          clear_exclusive{std::move(clear_exclusive_)} {}

    /// Did the manager cell or a bound table change since binding? Unreadable counts as moved.
    bool Moved() {
        using namespace OotGameDataTrace::Detail;
        if (!binding.manager) return false;
        const u64 m = binding.manager;
        if (!profile.manager_cell || Read64(memory, binding.base + profile.manager_cell) != m) return true;
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
        if (!profile.manager_cell) return false;
        const auto manager =
            OotGameDataTrace::Detail::Read64(memory, binding.base + profile.manager_cell);
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
        // Only keys from the reviewed policy reach here (ApplyPolicy and the frame poll).
        if (!change.key || (change.kind == Kind::Bool && change.key > 0xFFFFFFFFULL) ||
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
        if (change.kind == Kind::Rupee) {
            if (!profile.wallet_hash || change.key != profile.wallet_hash ||
                !RupeeDelta(change.value) || !binding.manager || !Callable()) return {};
            const auto entry = Verified(profile.rupee_entry);
            const auto pouch = profile.pouch_cell ? Read64(memory, binding.base + profile.pouch_cell)
                                                  : std::nullopt;
            if (!entry || !pouch || !*pouch || !Range(*pouch + 0x3BD4, 8)) return {};
            return ApplyTarget{true, false, 0, 0, *entry, binding.sentinel, binding.stack,
                               {*pouch, static_cast<u32>(static_cast<s32>(change.value)), RupeeReason}};
        }
        if (change.kind == Kind::Item) return ResolveItem(change);
        if (change.kind == Kind::Key) {
            // The game's own GameData Int add (a queued add record, like the Bool setter's write).
            if (!KeyGain(change.value) || !change.key || change.key > 0xFFFFFFFFULL ||
                !binding.manager || !Callable() || !ReadInt(static_cast<u32>(change.key))) return {};
            const auto entry = Verified(profile.int_add);
            const auto capacity = Read32(memory, binding.manager + 0xF0);
            const auto state = Read32(memory, binding.manager + 0x100);
            const auto buffer = Read64(memory, binding.manager + 0xF8);
            if (!entry || !capacity || !state || !buffer || !*capacity || *capacity > 0xFFFFF ||
                (*state & 0xFFFFF) >= *capacity || !Range(*buffer, u64{*capacity} * 0xC)) return {};
            return ApplyTarget{true, false, 0, 0, *entry, binding.sentinel, binding.stack,
                               {binding.manager, static_cast<u32>(static_cast<s32>(change.value)),
                                change.key}};
        }
        if (!ReadFlag(change).has_value() || !Callable()) return {};
        const bool flat = change.kind == Kind::Bool;
        const auto entry = Verified(flat ? profile.bool_setter : profile.bit_setter);
        if (!entry) return {};
        const auto capacity = Read32(memory, binding.manager + (flat ? 0x30 : 0xC68));
        const auto state = Read32(memory, binding.manager + (flat ? 0x40 : 0xC78));
        const u64 queue = flat ? binding.bool_queue : binding.queue;
        if (!capacity || !state || !*capacity || *capacity > 0xFFFFF ||
            (*state & 0xFFFFF) >= *capacity || !Range(queue, u64{*capacity} * 8)) return {};
        return ApplyTarget{true, false, 0, 0, *entry, binding.sentinel,
                           binding.stack, {binding.manager, 1, change.key}};
    }

    /// A committed GameData Int (the scalar table: index slots {key, idx} at manager+0xE0 with
    /// capacity +0xEC, records of 0x28 bytes at +0xD8 with the value at +0x1C, count +0xD0). Queued
    /// writes are not folded in: a change shows here once the game has committed it.
    std::optional<s32> ReadInt(u32 key) {
        using namespace OotGameDataTrace::Detail;
        if (!key || !binding.manager) return {};
        const u64 m = binding.manager;
        const auto table = Read64(memory, m + 0xE0);
        const auto capacity = Read32(memory, m + 0xEC);
        const auto count = Read32(memory, m + 0xD0);
        const auto data = Read64(memory, m + 0xD8);
        if (!table || !capacity || !count || !data || !*table || !*capacity || *capacity > 2'000'000 ||
            !*count || *count > 1'000'000 || !Range(*table, u64{*capacity} * 8) ||
            !Range(*data, u64{*count} * 0x28)) return {};
        u32 slot = key % *capacity;
        for (u32 probe = 0; probe < std::min(*capacity, 64U); ++probe) {
            const u64 address = *table + u64{slot} * 8;
            const u32 found = memory.Read32(address);
            if (!found) return {};
            if (found == key) {
                const u32 index = memory.Read32(address + 4);
                if (index >= *count) return {};
                return static_cast<s32>(memory.Read32(*data + u64{index} * 0x28 + 0x1C));
            }
            slot = (slot + 1) % *capacity;
        }
        return {};
    }

    /// Is the game where a give may run: no event in progress and every guard pointer live?
    bool ItemReady() {
        using namespace OotGameDataTrace::Detail;
        if (!profile.event_got) return false;
        const auto cell = Read64(memory, binding.base + profile.event_got);
        const auto events = cell ? Read64(memory, *cell) : std::nullopt;
        const auto running = events && *events ? Read64(memory, *events + 0x68) : std::nullopt;
        if (!running || *running) return false; // unreadable or an event is running
        for (const auto& [guard_cell, member] : profile.item_guards) {
            const auto object = Read64(memory, binding.base + guard_cell);
            if (!object || !*object) return false;
            const auto field = Read64(memory, *object + member);
            if (!field || !*field) return false;
        }
        return true;
    }

    std::optional<ApplyTarget> ResolveItem(const Change& change) {
        using namespace OotGameDataTrace::Detail;
        if (!ValidItem(change) || !binding.manager || !Callable() || binding.stack < ItemStackNeed ||
            !Range(binding.stack - ItemStackNeed, ItemStackNeed) || !ItemReady()) return {};
        // The remove call starts with the same words: the offset is what identifies the give.
        const auto entry = Verified(profile.item_entry);
        const auto pouch = profile.pouch_cell ? Read64(memory, binding.base + profile.pouch_cell)
                                              : std::nullopt;
        if (!entry || !pouch || !*pouch || !Range(*pouch, 0x10)) return {};
        const auto args = UnpackItem(change.key);
        const u64 sp = binding.stack - ItemFrame;
        ApplyTarget target{true, false, 0, 0, *entry, binding.sentinel, sp,
                           {*pouch, sp + 0x20, args.category, static_cast<u64>(change.value), args.get,
                            args.flags[0], args.flags[1], args.flags[2]}};
        target.stores = {{sp, args.flags[3]}, {sp + 8, 0}, {sp + 0x20, sp + 0x30}, {sp + 0x28, 0}};
        for (size_t i = 0; i < 8; ++i) { // 64 bytes: up to 63 characters and the NUL
            u64 word = 0;
            for (size_t j = 0; j < 8; ++j) {
                const size_t k = i * 8 + j;
                if (k < change.name.size()) word |= u64{static_cast<u8>(change.name[k])} << (8 * j);
            }
            target.stores.emplace_back(sp + 0x30 + i * 8, word);
        }
        return target;
    }

    /// For the ghost driver (class "pose"): the same entry check and call readiness as a grant.
    std::optional<u64> VerifiedEntry(const OotGameDataTrace::Entry& entry) { return Verified(entry); }
    bool CallReady() { return Callable() && binding.stack >= ItemStackNeed &&
                              Range(binding.stack - ItemStackNeed, ItemStackNeed); }

    bool OrFlag(const ApplyTarget&) { return false; }
    std::optional<Context> Capture() {
        return Context{jit.GetRegisters(), jit.GetVectors(), jit.GetPC(), jit.GetSP(), tls, tls_ro,
                       jit.GetPstate(), jit.GetFpcr(), jit.GetFpsr()};
    }
    bool Redirect(const ApplyTarget& target) {
        if (binding.invalid || target.sentinel != binding.sentinel || target.stack > binding.stack ||
            binding.stack - target.stack > ItemFrame || (target.stack & 15))
            return false;
        // The argument block lies in [target.stack, binding.stack): the private stack only.
        for (const auto& [address, value] : target.stores)
            if (address < target.stack || address + 8 > binding.stack || !Range(address, 8)) return false;
        for (const auto& [address, value] : target.stores) memory.Write64(address, value);
        // Reservations are discarded, as on a scheduler context switch: STXR must retry.
        jit.ClearExclusiveState();
        clear_exclusive();
        for (size_t i = 0; i < target.arguments.size(); ++i) jit.SetRegister(i, target.arguments[i]);
        if (target.s0_bits) {
            auto v = jit.GetVectors();
            v[0] = {u64{*target.s0_bits}, 0};
            jit.SetVectors(v);
        }
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
    /// The private stack and the sentinel breakpoint a synthetic call needs.
    bool Callable() {
        return binding.sentinel && binding.stack >= 0x4000 &&
               Range(binding.stack - 0x4000, 0x4000) &&
               OotGameDataTrace::Detail::Read32(memory, binding.sentinel) ==
                   OotGameDataTrace::Breakpoint;
    }
    /// The entry's guest address when its first words are the profile's, else nothing.
    std::optional<u64> Verified(const OotGameDataTrace::Entry& entry) {
        if (!entry || entry.words.size() > 16) return std::nullopt;
        const u64 address = binding.base + entry.offset;
        for (size_t i = 0; i < entry.words.size(); ++i) {
            if (OotGameDataTrace::Detail::Read32(memory, address + i * 4) != entry.words[i])
                return std::nullopt;
        }
        return address;
    }
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
    const OotGameDataTrace::Profile& profile;
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
    size_t Pending() const { return applier.Pending(); }
    template <typename Pred>
    size_t DropPending(Pred drop) { return applier.DropPending(drop); }
    const std::optional<Change>& LastGrant() const { return applier.LastGrant(); }
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
            if (!policy.classes.contains(kind) || EventKind(kind)) continue; // events: no flag to poll
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
