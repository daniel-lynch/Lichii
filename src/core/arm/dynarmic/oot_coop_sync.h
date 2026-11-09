// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <map>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/arm/dynarmic/oot_gamedata_trace.h"

namespace Kernel { class KernelCore; }
namespace Dynarmic::A64 { class Jit; }
namespace Core::Memory { class Memory; }

namespace Core::OotCoop {

inline constexpr u32 ProtocolVersion = 3;
inline constexpr u64 MaxSequence = (u64{1} << 53) - 1;
inline constexpr size_t MaxFrame = 1 << 20;
inline constexpr size_t QueueLimit = 4096;
inline constexpr size_t StateLimit = 4096;

// Pose is a class token only (EDEN_NX2_OOT_COOP_ALLOW "pose"): poses are never grants or state.
enum class Kind { Bool, Int, Enum, Quest, Bit, PouchAdd, PouchTotal, Rupee, Item, Pose, Key };

// Small keys (class "dungeon", wire "key_add"): a dungeon's key GAINS, as an additive event like a
// wallet delta. key = the dungeon's total-keys-ever-got Int key (from the private key file), value =
// the gain. The receiver adds the gain to that total and to the dungeon's current key count.
// Spending stays per-player (see PROTOCOL.md "Small keys").
inline constexpr s64 KeyGainLimit = 32;
[[nodiscard]] constexpr bool KeyGain(s64 delta) { return delta >= 1 && delta <= KeyGainLimit; }

/// One dungeon's keys: the total-got Int (observed, the wire key) and the current count Int.
struct DungeonKeys {
    u32 total{};
    u32 count{};
};

// Wallet deltas (class "rupee"): an additive event, never state. The key is the wallet's GameData
// hash from the private profile; the wire accepts any non-zero 32-bit key and the receiver's
// ApplyPolicy admits only its profile's wallet.
inline constexpr s64 RupeeLimit = 999;
[[nodiscard]] constexpr bool RupeeDelta(s64 delta) {
    return delta != 0 && delta >= -RupeeLimit && delta <= RupeeLimit;
}

// Item gives (class "item"): the game's own give-to-pouch call as the opener made it, replayed on
// the peer. An event like a wallet delta: deduplicated by origin/sequence, never team state.
// key packs the call's small arguments, value is the count, name the item's actor name.
inline constexpr s64 ItemCountLimit = 999;
inline constexpr size_t ItemNameLimit = 63;
struct ItemArgs {
    u32 category{}; // w2
    u32 get{};      // x4, the get type
    std::array<u8, 4> flags{}; // w5, w6, w7 and the stack byte
};
[[nodiscard]] constexpr u64 PackItem(const ItemArgs& args) {
    return u64{args.category & 0xFF} | (u64{args.get & 0xFF} << 8) | (u64{args.flags[0]} << 16) |
           (u64{args.flags[1]} << 24) | (u64{args.flags[2]} << 32) | (u64{args.flags[3]} << 40);
}
[[nodiscard]] constexpr ItemArgs UnpackItem(u64 key) {
    return {static_cast<u32>(key & 0xFF), static_cast<u32>((key >> 8) & 0xFF),
            {static_cast<u8>(key >> 16), static_cast<u8>(key >> 24), static_cast<u8>(key >> 32),
             static_cast<u8>(key >> 40)}};
}
[[nodiscard]] bool ItemName(std::string_view name);

struct Change {
    Kind kind{Kind::Bool};
    std::string origin;
    u64 seq{};
    u64 key{}; // Hash, challenge hash, or 64-bit actor hash; never a guest address.
    s64 value{}; // Boolean, int/enum, step index, or pouch count.
    u32 step{};
    std::string name; // Item only: the actor name passed to the give call
    World::Origin source; ///< Typed source identity, independent of sender origin/sequence.
    bool operator==(const Change&) const = default;
};

[[nodiscard]] nlohmann::json EncodeChange(const Change& change);
[[nodiscard]] std::optional<Change> DecodeChange(const nlohmann::json& packet,
                                                 bool snapshot = false);
[[nodiscard]] std::optional<nlohmann::json> DecodeLine(std::string_view line);
[[nodiscard]] std::string EncodeFrame(const nlohmann::json& packet);
[[nodiscard]] std::optional<u32> FrameLength(std::string_view header);
[[nodiscard]] bool WireKind(Kind kind);
/// An additive event (wallet delta, item give): applied once per origin/sequence, never state.
[[nodiscard]] constexpr bool EventKind(Kind kind) {
    return kind == Kind::Rupee || kind == Kind::Item || kind == Kind::Key;
}
/// A well-formed item give: name, count 1..999, category and get type below 16, top bits clear.
[[nodiscard]] bool ValidItem(const Change& change);
[[nodiscard]] std::optional<std::set<Kind>> DecodeAllowClasses(std::string_view text);

struct Endpoint {
    std::string host;
    u16 port{};
};
[[nodiscard]] std::optional<Endpoint> ParseEndpoint(std::string_view text);
[[nodiscard]] bool Gate(std::string_view endpoint, u64 program_id);

/// The "offsets" object of a private profile document for `build_id` (40 hex digits), or nothing
/// when that build is absent or any field is malformed. Numbers may be JSON integers or "0x" strings.
[[nodiscard]] std::optional<OotGameDataTrace::Profile> DecodeProfile(const nlohmann::json& document,
                                                                     std::string_view build_id);
/// DecodeProfile of the EDEN_NX2_OOT_COOP_KEYS file; `why` says what failed.
[[nodiscard]] std::shared_ptr<const OotGameDataTrace::Profile> LoadProfile(std::string_view build_id,
                                                                           std::string* why = nullptr);
[[nodiscard]] bool Enabled();

struct Hello {
    std::string build_id; // the running main build; RegisterMain sets it
    std::string seed_hash{"0"};
    std::string policy_id{"or-v2"};
    std::string room{"default"};
    std::string team{"default"};
    std::string client_id;
    std::string name{"Player"};
    std::optional<World::Wallet> world_wallet;
};
[[nodiscard]] nlohmann::json EncodeHello(const Hello& hello);
[[nodiscard]] bool MatchesHello(const nlohmann::json& packet, const Hello& hello);
[[nodiscard]] std::optional<std::set<u64>> DecodeDenyList(const nlohmann::json& packet);

[[nodiscard]] std::optional<std::set<std::pair<Kind, u64>>> DecodeKeyPolicy(
    const nlohmann::json& packet);

/// The key file's optional C13 sections (DecodeKeyPolicy skips them):
///   "deny": {"bool": [keys]}   per-player Bools, never shared (merged into the deny set)
///   "dungeon": {"bool": [keys], "keys": [[total, count], ...]}
///     map/compass/boss-key Bools (added to the reviewed Bool set when class dungeon is on; they
///     travel as bool_set) and each dungeon's small-key Int pair (class dungeon, key_add)
struct KeyExtras {
    std::set<u64> deny;
    std::vector<u32> dungeon_bools;
    std::vector<DungeonKeys> dungeon_keys;
};
inline constexpr size_t DenyLimit = 16384;
[[nodiscard]] std::optional<KeyExtras> DecodeKeyExtras(const nlohmann::json& document);

/// Small-key observation for one dungeon: a gain is the larger rise of its total and its count
/// since the last frame, minus what our own applied key_add events explain (the credit).
struct KeyWatch {
    std::optional<s32> total, count;
    s64 credit{};
    /// The gain to forward now (0: none). Unreadable values reset the watch (no forward).
    s64 Step(std::optional<s32> new_total, std::optional<s32> new_count) {
        if (!new_total || !new_count) {
            total.reset();
            count.reset();
            return 0;
        }
        s64 gain = 0;
        if (total && count) {
            gain = std::max<s64>({0, s64{*new_total} - *total, s64{*new_count} - *count});
        }
        total = new_total;
        count = new_count;
        const s64 explained = std::min(gain, credit);
        credit -= explained;
        return gain - explained;
    }
};

using HighWater = std::map<std::string, u64, std::less<>>;

/// The same-chest race (C13): two players open one chest before either's chest Bool arrives. Every
/// client applies the gives of one chest from exactly ONE opener: itself when it opened the chest
/// first (or had already given locally in that event), else the first peer that claimed it (its
/// bool_set or its tagged give, whichever arrives first). Local gives are held until their event
/// ends and then sent tagged with the chest the event opened.
class ChestClaims {
public:
    enum class Open { Ours, SkipLocal, TakeOver };
    /// A chest of ours really opened (the actor flag setter) during `event`. Ours: first claim.
    /// SkipLocal: a peer owns it and we have not given yet in this event: skip our gives (the peer's
    /// copy is the one). TakeOver: we already gave locally: we own it, drop the peer's pending gives.
    Open LocalOpen(World::Origin key, u64 event) {
        auto& slot = event_key[event];
        slot = slot == World::Origin{} || slot == key ? key : Ambiguous;
        const auto it = owner.find(key);
        if (it == owner.end() || it->second.empty()) {
            owner[key] = {};
            return Open::Ours;
        }
        if (held[event] == 0) {
            skip.insert(event);
            return Open::SkipLocal;
        }
        it->second.clear();
        return Open::TakeOver;
    }
    /// A peer's chest Bool (its open): the first claimant owns the chest.
    void RemoteClaim(World::Origin key, const std::string& origin) {
        if (key && !origin.empty()) owner.try_emplace(key, origin);
    }
    /// An inbound give: apply it? Untagged gives always apply; a tagged one only from the owner
    /// (the sender becomes the owner of an unclaimed chest).
    bool AcceptRemote(const Change& give) {
        if (!give.source) return true;
        const auto [it, fresh] = owner.try_emplace(give.source, give.origin);
        return fresh || (!it->second.empty() && it->second == give.origin);
    }
    /// A local, forwardable top-level give during `event` (0 = none): skip it, or hold it.
    bool SkipLocal(u64 event) const { return event && skip.contains(event); }
    void Hold(u64 event) { ++held[event]; }
    /// The event ended: the chest its gives came from (0 = none or more than one), and forget it.
    World::Origin End(u64 event) {
        const auto it = event_key.find(event);
        const auto key = it == event_key.end() || it->second == Ambiguous ? World::Origin{} : it->second;
        event_key.erase(event);
        held.erase(event);
        skip.erase(event);
        return key;
    }
    std::string_view Owner(World::Origin key) const {
        const auto it = owner.find(key);
        if (it == owner.end()) return "none";
        if (it->second.empty()) return "us";
        return it->second;
    }
    size_t Size() const { return owner.size(); }

private:
    static constexpr World::Origin Ambiguous{World::OriginKind::None, 1, 0};
    std::map<World::Origin, std::string> owner; ///< "" = we opened it
    std::map<u64, World::Origin> event_key;
    std::map<u64, u32> held;
    std::set<u64> skip;
};

// Protocol state only. A successful Merge does NOT mean anything was applied to the guest.
class State {
public:
    explicit State(std::set<u64> denied_ = {}) : denied{std::move(denied_)} {}
    bool Merge(const Change& change, bool deduplicate = true);
    bool AdoptSeen(const HighWater& high_water);
    [[nodiscard]] std::optional<Change> Find(Kind kind, u64 key) const;
    [[nodiscard]] std::vector<Change> Snapshot() const;

private:
    std::set<u64> denied;
    std::map<std::pair<Kind, u64>, Change> values;
    HighWater seen;
};

[[nodiscard]] bool ShouldApply(const Change& change, const State& observed);

class Session {
public:
    Session(Kernel::KernelCore& kernel, Kernel::KProcess* owner_, u64 base_,
            std::shared_ptr<const OotGameDataTrace::Profile> profile_, Endpoint endpoint, Hello hello,
            std::set<u64> denied, std::set<Kind> allowed_classes = {},
            std::set<std::pair<Kind, u64>> allowed_keys = {});
    ~Session();
    void Stop();
    void Observe(Change change);
    bool Hook(OotGameDataTrace::Point point, u64 pc, Dynarmic::A64::Jit& jit,
              Memory::Memory& memory, u64 thread, u64& tls, u64& tls_ro,
              std::function<void()> clear_exclusive);
    bool IsSentinel(u64 pc) const;
    /// The points this session traps: the co-op set, plus the actor flag setter when the profile
    /// maps chest members (the same-chest race guard).
    bool Traps(OotGameDataTrace::Point point) const {
        return OotGameDataTrace::CoopPoint(point) ||
               (point == OotGameDataTrace::Point::ActorFlagSet && !profile->chest_members.empty());
    }
    /// Class dungeon: the small-key Int pairs to watch and apply (from the key file).
    void SetDungeonKeys(std::vector<DungeonKeys> keys);
    Kernel::KProcess* const owner;
    const u64 base;
    const std::shared_ptr<const OotGameDataTrace::Profile> profile; ///< Never null

private:
    /// The give-to-pouch entry trapped on this (opener's) game: log it, forward chest gives.
    void ObserveItem(Dynarmic::A64::Jit& jit, Memory::Memory& memory, u64 thread);
    /// The wallet entry trapped here: forward a reviewed gameplay caller's signed delta (class rupee).
    void ObserveRupee(Dynarmic::A64::Jit& jit, u64 thread);
    /// The actor flag setter trapped here: a chest of the profile really opening (claim it).
    void ObserveActorFlag(Dynarmic::A64::Jit& jit, Memory::Memory& memory);
    /// The running event's context (EventMgr+0x68), 0 when none, nothing when unreadable.
    std::optional<u64> EventContext(Memory::Memory& memory) const;
    struct Impl;
    /// Class "pose": send our pose, draw the peers' ghosts (one call at most, after the grants).
    template <typename Backend, typename Log>
    void FramePose(Backend& backend, Memory::Memory& memory, Log log, bool grant_calling);
    std::unique_ptr<Impl> impl;
};

// Reset stops the previous transport even when a JIT still owns its shared session.
void BeginSession();
void RegisterMain(Kernel::KernelCore& kernel, Kernel::KProcess* owner, u64 program_id, u64 base,
                  std::string_view build_id);
[[nodiscard]] std::shared_ptr<Session> FindSession(const Kernel::KProcess* owner);

// Reuse the quest layout already present in the read-only trace.
template <typename Memory>
std::optional<u32> StepIndex(Memory& memory, u64 manager, u32 challenge, u32 step) {
    using namespace OotGameDataTrace::Detail;
    for (const u64 table : {u64{0x40}, u64{0x50}, u64{0x60}}) {
        const auto count = Read32(memory, manager + table);
        const auto data = Read64(memory, manager + table + 8);
        if (!count || !data || *count > 4096) {
            continue;
        }
        for (u32 i = 0; i < *count; ++i) {
            const u64 record = *data + u64{i} * 0x160;
            if (Read32(memory, record + 8) != challenge) {
                continue;
            }
            const u64 state = record + 0x18;
            const auto current = Read32(memory, state + 0x64);
            const auto length = Read32(memory, state + 0x68);
            const auto steps = Read64(memory, state + 0x70);
            if (!current || !length || !steps || *length > 256 || *current >= *length) {
                return std::nullopt;
            }
            for (u32 index = 0; index < *length; ++index) {
                if (Read32(memory, *steps + u64{index} * 0x90 + 0x78) == step) {
                    return index > *current ? std::optional<u32>{index} : std::nullopt;
                }
            }
            return std::nullopt;
        }
    }
    return std::nullopt;
}

} // namespace Core::OotCoop
