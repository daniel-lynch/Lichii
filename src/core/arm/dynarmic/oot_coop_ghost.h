// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Co-op "ghost" (class "pose", protocol v3): each player's game sends its own pose; every peer in
// the same scene is drawn as a statue puppet created through the game's own actor manager and
// moved with the actor's own set-transform call from the frame drain. No joints, no animation.
// Everything game-derived (offsets, keys, entry words, actor names) comes from the private profile.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/arm/dynarmic/oot_gamedata_trace.h"

namespace Core::OotCoop {

struct Pose {
    u32 t{};          ///< The sender's frame counter (wraps)
    std::string banc; ///< The sender's scene; empty: no save, title screen or unreadable
    bool child{};
    bool loading{};   ///< Scene loading, or the position has not settled since the scene changed
    std::array<float, 3> pos{};
    float yaw{};      ///< Degrees; facing = atan2(m02, m22) of the rotation
    // Animation hint for a later animated receiver (v1 draws a statue and ignores it). Inferred
    // from the sender's speed: the game's own command table says "Move" for both walk and run.
    std::string anim{"idle"}; ///< "idle", "walk" or "run"
    u32 anim_seq{};           ///< Bumped on every category change: a receiver restarts only then
    float rate{};             ///< Speed relative to the category's nominal speed, 0..10
    float phase{};            ///< Cycle position in [0, 1): distance (or time when idle) per stride
    bool operator==(const Pose&) const = default;
};
[[nodiscard]] bool AnimName(std::string_view name);

inline constexpr size_t PoseFrameLimit = 256;
inline constexpr float PoseCoordinateLimit = 100000.0f;
inline constexpr size_t GhostSlots = 4; ///< Peers drawn at once

[[nodiscard]] bool BancName(std::string_view name);
/// The client form: no origin, no sequence.
[[nodiscard]] nlohmann::json EncodePose(const Pose& pose);
/// `origin` empty: the client form (relay input); otherwise the forwarded form, whose origin is
/// returned through `origin`. Unknown fields, bad names or non-finite numbers reject it.
[[nodiscard]] std::optional<Pose> DecodePose(const nlohmann::json& packet, std::string* origin = nullptr);

/// Linear position, shortest-arc yaw; `f` in [0, 1].
[[nodiscard]] Pose Blend(const Pose& a, const Pose& b, float f);

/// Matrix34f rows [m00 m01 m02 m03] [m10 m11 m12 m13] [m20 m21 m22 m23]: a yaw about +Y and the
/// translation, the same convention the sender's yaw is read with (yaw = atan2(m02, m22)).
[[nodiscard]] inline std::array<float, 12> YawMatrix(const std::array<float, 3>& p, float yaw) {
    const double r = static_cast<double>(yaw) * 3.14159265358979323846 / 180.0;
    const float c = static_cast<float>(std::cos(r)), s = static_cast<float>(std::sin(r));
    return {c, 0.0f, s, p[0], 0.0f, 1.0f, 0.0f, p[1], -s, 0.0f, c, p[2]};
}

/// One recent pose per arrival, newest last, for the receiver's ~100 ms interpolation delay.
struct PeerTrack {
    std::deque<std::pair<u64, Pose>> samples; ///< (arrival ms, pose)
    u64 last_ms{};
    bool online{true};

    void Add(u64 now_ms, Pose pose) {
        samples.emplace_back(now_ms, std::move(pose));
        while (samples.size() > 8) samples.pop_front();
        last_ms = now_ms;
    }
    /// The pose to draw at `now_ms - delay_ms`; nothing when no sample is newer than `stale_ms`.
    [[nodiscard]] std::optional<Pose> At(u64 now_ms, u64 delay_ms = 100, u64 stale_ms = 5000) const {
        if (!online || samples.empty() || now_ms - samples.back().first > stale_ms) return {};
        const u64 render = now_ms > delay_ms ? now_ms - delay_ms : 0;
        if (render >= samples.back().first || samples.size() == 1) return samples.back().second;
        if (render <= samples.front().first) return samples.front().second;
        for (size_t i = 1; i < samples.size(); ++i) {
            const auto& [ta, a] = samples[i - 1];
            const auto& [tb, b] = samples[i];
            if (render > tb) continue;
            if (a.banc != b.banc || a.loading != b.loading || a.child != b.child || tb <= ta) return b;
            return Blend(a, b, static_cast<float>(render - ta) / static_cast<float>(tb - ta));
        }
        return samples.back().second;
    }
};

namespace GhostDetail {
using OotGameDataTrace::Detail::Read32;
using OotGameDataTrace::Detail::Read64;

/// GameData open-addressed lookup ({u32 count, data*, hashes*, .., u32 capacity} at `table`; slots
/// {u32 key, u32 index}), bounded like the flag reader: the record address or nothing.
template <typename Memory>
std::optional<u64> Record(Memory& memory, u64 manager, u64 table, u64 stride, u32 key) {
    const auto count = Read32(memory, manager + table);
    const auto data = Read64(memory, manager + table + 8);
    const auto hashes = Read64(memory, manager + table + 0x10);
    const auto capacity = Read32(memory, manager + table + 0x1C);
    if (!key || !count || !data || !hashes || !capacity || !*data || !*hashes || !*capacity ||
        *capacity > 2'000'000)
        return {};
    u32 slot = key % *capacity;
    for (u32 probe = 0; probe < std::min(*capacity, 64U); ++probe) {
        const auto k = Read32(memory, *hashes + u64{slot} * 8);
        if (!k || !*k) return {};
        if (*k == key) {
            const auto index = Read32(memory, *hashes + u64{slot} * 8 + 4);
            if (!index || *index >= *count) return {};
            return *data + u64{*index} * stride;
        }
        slot = (slot + 1) % *capacity;
    }
    return {};
}

template <typename Memory>
std::optional<bool> Flag(Memory& memory, u64 manager, u32 key) {
    const auto record = Record(memory, manager, 0x10, 0x18, key); // the flat Bool table
    if (!record || !memory.IsValidVirtualAddressRange(*record + 0x16, 1)) return {};
    const u8 value = memory.Read8(*record + 0x16);
    return value <= 1 ? std::optional<bool>{value != 0} : std::nullopt;
}

template <typename Memory>
std::optional<std::string> Text(Memory& memory, u64 manager, const std::array<u64, 3>& layout, u32 key) {
    const auto record = Record(memory, manager, layout[0], layout[1], key);
    const auto text = record ? Read64(memory, *record + layout[2]) : std::nullopt;
    if (!text) return {};
    std::string out;
    if (!*text) return out;
    for (u64 i = 0; i < 64; ++i) {
        if (!memory.IsValidVirtualAddressRange(*text + i, 1)) return {};
        const char c = static_cast<char>(memory.Read8(*text + i));
        if (!c) return out;
        out.push_back(c);
    }
    return {};
}

inline float Float(u32 bits) {
    float f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}
} // namespace GhostDetail

/// The local pose at the frame hook: pure reads. `banc` is empty when anything is unreadable.
template <typename Memory>
Pose ReadLocalPose(Memory& memory, u64 base, u64 manager, const OotGameDataTrace::GhostOffsets& g,
                   u32 tick) {
    using namespace GhostDetail;
    Pose pose;
    pose.t = tick;
    const auto info = Read64(memory, base + g.playing_info_cell);
    const auto banc = manager ? Text(memory, manager, g.str64, g.banc_hash) : std::nullopt;
    const auto child = manager ? Flag(memory, manager, g.child_hash) : std::nullopt;
    const auto loading = manager ? Flag(memory, manager, g.loading_hash) : std::nullopt;
    if (!info || !*info || !banc || !child || !loading || !BancName(*banc)) return pose;
    std::array<float, 3> p{};
    for (size_t i = 0; i < 3; ++i) {
        const auto bits = Read32(memory, *info + g.pos + i * 4);
        if (!bits) return pose;
        p[i] = Float(*bits);
        if (!std::isfinite(p[i]) || std::fabs(p[i]) > PoseCoordinateLimit) return pose;
    }
    const auto m02 = Read32(memory, *info + g.rot + 8);
    const auto m22 = Read32(memory, *info + g.rot + 0x20);
    if (!m02 || !m22 || !std::isfinite(Float(*m02)) || !std::isfinite(Float(*m22))) return pose;
    pose.banc = *banc;
    pose.child = *child;
    pose.loading = *loading;
    pose.pos = p;
    pose.yaw = static_cast<float>(std::atan2(Float(*m02), Float(*m22)) * 180.0 / 3.14159265358979323846);
    return pose;
}

/// Sender pacing: about 15 Hz while something changes, a 1 Hz keepalive while still, and
/// `loading` held until the position moves (or ~2 s pass) after a scene change: the player-info
/// position is still the old scene's for a moment after the load finishes.
class PoseSender {
public:
    /// The pose to send now, if any. `now_ms` is a host clock.
    std::optional<Pose> Sample(Pose pose, u64 now_ms) {
        Animate(pose, now_ms);
        if (pose.banc != scene) {
            scene = pose.banc;
            settling = true;
            settle_pos = last_pos;
            settle_start = now_ms;
        }
        if (settling && !pose.loading && !pose.banc.empty() &&
            (Far(pose.pos, settle_pos, 0.01f) || now_ms - settle_start > 2000))
            settling = false;
        last_pos = pose.pos;
        if (settling && !pose.banc.empty()) pose.loading = true;
        if (sent && now_ms - sent_ms < 66) return {};
        const bool changed = !sent || pose.banc != last.banc || pose.child != last.child ||
                             pose.anim_seq != last.anim_seq ||
                             pose.loading != last.loading || Far(pose.pos, last.pos, 0.01f) ||
                             std::fabs(pose.yaw - last.yaw) > 0.5f;
        if (!changed && now_ms - sent_ms < 1000) return {};
        sent = true;
        sent_ms = now_ms;
        last = pose;
        return pose;
    }

    static constexpr float IdleBelow = 0.3f, RunFrom = 2.6f; ///< metres per second
    static constexpr float WalkSpeed = 1.5f, RunSpeed = 4.5f, WalkStride = 1.2f, RunStride = 2.4f;

private:
    /// Speed over at least 100 ms of samples; category with hysteresis-free thresholds; the phase
    /// advances by distance per stride (by time while idle) so a receiver never restarts per packet.
    void Animate(Pose& pose, u64 now_ms) {
        if (!have_anim || pose.banc != anim_banc) {
            have_anim = true;
            anim_banc = pose.banc;
            anim_pos = pose.pos;
            anim_ms = now_ms;
            speed = 0;
        } else if (now_ms - anim_ms >= 100) {
            const float dx = pose.pos[0] - anim_pos[0], dz = pose.pos[2] - anim_pos[2];
            const float dist = std::sqrt(dx * dx + dz * dz);
            const float dt = static_cast<float>(now_ms - anim_ms) / 1000.0f;
            speed = dist / dt;
            const std::string cat = speed < IdleBelow ? "idle" : speed < RunFrom ? "walk" : "run";
            if (cat != category) {
                category = cat;
                ++seq;
            }
            phase += category == "idle" ? dt / 2.0f : dist / (category == "walk" ? WalkStride : RunStride);
            phase -= std::floor(phase);
            anim_pos = pose.pos;
            anim_ms = now_ms;
        }
        pose.anim = category;
        pose.anim_seq = seq;
        pose.rate = std::clamp(category == "idle" ? 1.0f : speed / (category == "walk" ? WalkSpeed : RunSpeed),
                               0.0f, 10.0f);
        pose.phase = phase >= 1.0f ? 0.0f : phase;
    }
    std::string category{"idle"}, anim_banc;
    std::array<float, 3> anim_pos{};
    u64 anim_ms{};
    u32 seq{};
    float speed{}, phase{};
    bool have_anim{};

    static bool Far(const std::array<float, 3>& a, const std::array<float, 3>& b, float d) {
        return std::fabs(a[0] - b[0]) > d || std::fabs(a[1] - b[1]) > d || std::fabs(a[2] - b[2]) > d;
    }
    std::string scene;
    std::array<float, 3> last_pos{}, settle_pos{};
    u64 settle_start{}, sent_ms{};
    bool settling{}, sent{};
    Pose last;
};

/// A peer as the frame drain sees it: the interpolated pose, or nothing (stale, offline, gone).
struct PeerView {
    std::string origin;
    std::optional<Pose> pose;
};

enum class GhostOp { None, Create, Get, Release, Move, Play, Alpha, Tint };

/// Per-peer puppet state machine. One synthetic call at most per frame, and only when the grant
/// drain has not started one. Port:
///   std::optional<u64> R64(u64); std::optional<u32> R32(u64); bool Write(u64, const void*, size_t);
///   std::optional<u64> Verified(const OotGameDataTrace::Entry&);
///   bool Call(u64 entry, const std::array<u64, 8>& args, std::optional<float> s0 = {});
///                                                    // capture, redirect; sentinel return
template <typename Port>
class GhostDriver {
public:
    enum class State { None, Pending, Live };
    struct Slot {
        std::string origin;
        State state{State::None};
        u64 actor{}, vtable{}, record{};
        u32 generation{};
        std::vector<u64> fields;
        std::string banc;
        bool parked{};
        std::array<float, 4> drawn{}; ///< x, y, z, yaw last sent to set-transform
        u64 retry_tick{}, pending_tick{};
        bool pending_logged{};
        bool adult_fallback{}; ///< Adult peer, child puppet (no adult puppet in the profile)
        bool fallback{};       ///< The primary puppet's create failed: the fallback is in use
        bool alpha_done{};
        bool tint_done{};
        bool placed{};         ///< At least one set-transform since it went live
        bool abandon{};        ///< The watchdog gave up on it: park it once, then re-create
        u32 abandons{};        ///< Watchdog re-creates in this scene (bounded)
        u32 watch_bad{}, hide_bad{};
        u64 watch_tick{}, absent_since{};
        std::optional<u32> hide_seen; ///< Last hide-reason byte logged
        std::string played;    ///< Pose category whose command was last played ("" = none yet)
        u32 played_seq{};
        bool play_logged{};
    };
    static constexpr u64 SlotSize = 0x200;
    static constexpr u64 CreateRetry = 1800; ///< frames after a refused/failed create
    static constexpr float ParkY = -10000.0f;
    static constexpr u64 StatusEvery = 1800;  ///< frames between status lines per ghost
    static constexpr u64 WatchEvery = 30;     ///< frames between watchdog reads
    static constexpr u32 WatchStrikes = 3;    ///< consecutive bad reads before a re-create
    static constexpr u32 AbandonLimit = 4;    ///< watchdog re-creates per slot per scene
    static constexpr float WatchFar = 2.0f;   ///< metres between drawn and read-back position
    static constexpr u64 OrphanAfter = 600;   ///< frames a slot's peer is gone before reuse

    GhostDriver(const OotGameDataTrace::GhostOffsets& g_, u64 base_) : g{g_}, base{base_} {}

    bool Calling() const { return op != GhostOp::None; }
    const std::array<Slot, GhostSlots>& Slots() const { return slots; }

    /// Forget every actor without a call (the game deletes them with the scene / save).
    template <typename Log>
    void DropAll(Log log, std::string_view why) {
        for (auto& slot : slots) {
            if (slot.state == State::Live) Line(log, slot, fmt::format("dropped ({})", why));
            if (slot.state != State::Pending) slot.state = State::None;
            slot.actor = 0;
        }
    }

    /// `block`: GhostSlots * SlotSize bytes of persistent guest memory we own. Returns true when
    /// a call was started (the caller must not run anything else on this thread this frame).
    template <typename Log>
    bool Frame(Port& port, u64 block, const Pose& local, const std::vector<PeerView>& peers, Log log) {
        if (Calling() || !block) return false;
        ++tick;
        const bool ready = !local.banc.empty() && !local.loading;
        // How long each slot's peer has been missing from the list (relay gone, peer offline).
        for (auto& slot : slots) {
            if (slot.origin.empty()) continue;
            const bool listed = std::any_of(peers.begin(), peers.end(),
                                            [&](const PeerView& p) { return p.origin == slot.origin; });
            if (listed) slot.absent_since = 0;
            else if (!slot.absent_since) slot.absent_since = tick;
        }
        // Assign slots to peers with a pose; a peer keeps its slot while it is listed. A peer
        // with no slot takes a free one, else a live ghost whose peer has been gone a while (a
        // peer that came back with a new identity, e.g. after restarting): the actor is kept.
        for (const auto& peer : peers) {
            if (!peer.pose || Find(peer.origin)) continue;
            Slot* free = nullptr;
            for (auto& slot : slots) {
                if (slot.origin.empty()) {
                    free = &slot;
                    break;
                }
            }
            if (free) {
                *free = Slot{};
                free->origin = peer.origin;
                Line(log, *free, "assigned (new peer)");
                continue;
            }
            for (auto& slot : slots) {
                if (slot.state == State::Live && slot.absent_since && tick - slot.absent_since >= OrphanAfter) {
                    Line(log, slot, fmt::format("reassigned to peer {} (its peer is gone; actor kept)",
                                                peer.origin.substr(0, 8)));
                    slot.origin = peer.origin;
                    slot.absent_since = 0;
                    slot.played.clear();
                    slot.play_logged = false;
                    break;
                }
            }
        }
        // Validate without calling: a dead or out-of-scene actor is forgotten, never touched.
        for (auto& slot : slots) {
            if (slot.state != State::Live) continue;
            if (!ready || local.banc != slot.banc) {
                Line(log, slot, fmt::format("dropped (local scene {} {})", local.banc.empty() ? "-" : local.banc,
                                            local.loading ? "loading" : "changed"));
                slot.state = State::None;
                slot.actor = 0;
                slot.abandons = 0; // a new scene: the watchdog may re-create again
            } else if (const auto why = Dead(port, slot)) {
                Drop(log, slot, *why);
            } else {
                Watch(port, slot, log);
            }
        }
        if (tick % StatusEvery == 0) Status(port, local, peers, log);
        for (size_t n = 0; n < GhostSlots; ++n) {
            const size_t i = (next + n) % GhostSlots;
            auto& slot = slots[i];
            if (slot.origin.empty()) continue;
            const u64 at = block + i * SlotSize;
            const PeerView* peer = nullptr;
            for (const auto& p : peers) if (p.origin == slot.origin) peer = &p;
            const bool visible = ready && peer && peer->pose && !peer->pose->loading &&
                                 peer->pose->banc == local.banc;
            if (slot.state == State::None) {
                if (!peer) { slot.origin.clear(); continue; } // gone and nothing to clean up
                if (!visible || tick < slot.retry_tick) continue;
                if (StartCreate(port, at, slot, *peer->pose, local.banc, log)) return Started(i, GhostOp::Create);
                slot.retry_tick = tick + CreateRetry;
                continue;
            }
            if (slot.state == State::Pending) {
                const auto handle = port.R64(at);
                const auto record = handle && *handle ? port.R32(*handle) : std::nullopt;
                if (!handle || !*handle || !record) {
                    Line(log, slot, "create handle cleared; retrying later");
                    slot.state = State::None;
                    slot.retry_tick = tick + CreateRetry;
                    continue;
                }
                const u32 state = *record & 0xFFFF;
                if (state == 3 && ready && local.banc == slot.banc) {
                    if (Start(port, g.get, {at, 1, 0}, i, GhostOp::Get)) return true;
                } else if (state == 3 || state == 4) {
                    if (state == 4) {
                        Line(log, slot, "create failed (state 4)");
                        Fallback(log, slot);
                    }
                    if (Start(port, g.release, {at}, i, GhostOp::Release)) return true;
                } else if (!slot.pending_logged && tick - slot.pending_tick > 600) {
                    Line(log, slot, fmt::format("create still pending (state {})", state));
                    slot.pending_logged = true;
                }
                continue;
            }
            // Live
            std::array<float, 4> target;
            if (slot.abandon) {
                // The watchdog gave up on this actor: park it out of sight once, then forget it.
                if (const auto why = Dead(port, slot)) {
                    Drop(log, slot, *why);
                    continue;
                }
                const std::array<float, 4> park{slot.drawn[0], ParkY, slot.drawn[2], slot.drawn[3]};
                const auto m = YawMatrix({park[0], park[1], park[2]}, park[3]);
                if (port.Write(at + 0x10, m.data(), sizeof(m)) &&
                    Start(port, g.set_mtx, {slot.actor, at + 0x10, 0}, i, GhostOp::Move))
                    return true;
                Abandoned(log, slot);
                continue;
            }
            if (visible && !slot.parked && g.alpha_set && !slot.fallback && !slot.alpha_done &&
                g.alpha_percent >= 100) {
                slot.alpha_done = true; // opaque: the puppet's own alpha is already 1
                Line(log, slot, "alpha 100% (opaque, no call)");
            }
            if (visible && !slot.parked && g.alpha_set && !slot.fallback && !slot.alpha_done) {
                slot.alpha_done = true; // once per puppet, whatever the result
                const auto model = Model(port, slot);
                if (model) {
                    if (const auto why = Dead(port, slot)) {
                        Drop(log, slot, *why);
                        continue;
                    }
                    if (Start(port, g.alpha_set, {*model}, i, GhostOp::Alpha,
                              static_cast<float>(g.alpha_percent) / 100.0f))
                        return true;
                } else {
                    Line(log, slot, "alpha skipped (no model component)");
                }
                continue;
            }
            if (visible && !slot.parked && g.tint_set && !g.tint_colors.empty() && !slot.fallback &&
                !slot.tint_done) {
                slot.tint_done = true; // once per puppet, whatever the result
                const auto model = Model(port, slot);
                const auto& rgba = g.tint_colors[i % g.tint_colors.size()];
                if (model) {
                    if (const auto why = Dead(port, slot)) {
                        Drop(log, slot, *why);
                        continue;
                    }
                    if (port.Write(at + TintAt, rgba.data(), sizeof(rgba)) &&
                        Start(port, g.tint_set, {*model, at + TintAt}, i, GhostOp::Tint))
                        return true;
                } else {
                    Line(log, slot, "tint skipped (no model component)");
                }
                continue;
            }
            if (visible && !slot.parked && g.anim_play && !slot.fallback) {
                // Animation category path: play the category's command once per change (the
                // play call restarts the clip; it is never re-issued per packet).
                const auto& p = *peer->pose;
                const std::string& command = p.anim == "walk" ? g.anim_walk : p.anim == "run" ? g.anim_run
                                                                                              : g.anim_idle;
                if (!command.empty() && (slot.played != p.anim || slot.played_seq != p.anim_seq)) {
                    if (const auto why = Dead(port, slot)) {
                        Drop(log, slot, *why);
                        continue;
                    }
                    // Request at +0x140 (0x30 bytes), command at +0x180 (64 bytes).
                    std::array<u8, 0x70> req{};
                    const u64 cmd_at = at + 0x180;
                    const float rate = std::clamp(p.rate > 0.05f ? p.rate : 1.0f, 0.25f, 4.0f);
                    const float minus_one = -1.0f;
                    std::memcpy(req.data(), &slot.actor, 8);
                    std::memcpy(req.data() + 8, &cmd_at, 8);
                    std::memcpy(req.data() + 0x14, &rate, 4);
                    std::memcpy(req.data() + 0x18, &minus_one, 4);
                    std::memcpy(req.data() + 0x1C, &minus_one, 4);
                    std::memcpy(req.data() + 0x40, command.data(), std::min<size_t>(command.size(), 63));
                    slot.played = p.anim;
                    slot.played_seq = p.anim_seq;
                    if (port.Write(at + 0x140, req.data(), req.size()) &&
                        Start(port, g.anim_play, {at + 0x140}, i, GhostOp::Play)) {
                        slot_name = command;
                        return true;
                    }
                    continue;
                }
            }
            if (visible) {
                const auto& p = *peer->pose;
                target = {p.pos[0], p.pos[1], p.pos[2], p.yaw};
                const bool moved = slot.parked || std::fabs(target[0] - slot.drawn[0]) > 0.01f ||
                                   std::fabs(target[1] - slot.drawn[1]) > 0.01f ||
                                   std::fabs(target[2] - slot.drawn[2]) > 0.01f ||
                                   std::fabs(target[3] - slot.drawn[3]) > 0.5f;
                if (!moved) continue;
                if (slot.parked)
                    Line(log, slot, fmt::format("shown (peer in {} at ({:.2f}, {:.2f}, {:.2f}))", p.banc, p.pos[0],
                                                p.pos[1], p.pos[2]));
                slot.parked = false;
            } else {
                if (slot.parked) continue;
                target = {slot.drawn[0], ParkY, slot.drawn[2], slot.drawn[3]};
                const std::string why =
                    !peer         ? std::string{"peer gone"}
                    : !peer->pose ? std::string{"peer stale"}
                    : !ready      ? fmt::format("local scene {} not ready", local.banc.empty() ? "-" : local.banc)
                    : peer->pose->loading
                        ? fmt::format("peer loading {}", peer->pose->banc.empty() ? "-" : peer->pose->banc)
                        : fmt::format("peer elsewhere: peer in {}, we are in {}",
                                      peer->pose->banc.empty() ? "-" : peer->pose->banc, local.banc);
                Line(log, slot, fmt::format("parked ({})", why));
                slot.parked = true;
            }
            // Re-check right before the call: the checks above ran for every slot first. This is
            // the same check-then-use the game's own link check does (no reference is held across
            // the use there either; see HOOKS.md "Ghost lifetime").
            if (const auto why = Dead(port, slot)) {
                Drop(log, slot, *why);
                continue;
            }
            const auto m = YawMatrix({target[0], target[1], target[2]}, target[3]);
            if (!port.Write(at + 0x10, m.data(), sizeof(m))) continue;
            if (Start(port, g.set_mtx, {slot.actor, at + 0x10, 0}, i, GhostOp::Move)) {
                slot.drawn = target;
                slot.placed = true;
                ++moves;
                return true;
            }
        }
        return false;
    }

    /// The call returned to the sentinel (the context is already restored); x0 is its result.
    template <typename Log>
    void Returned(Port& port, u64 block, u64 x0, Log log) {
        const GhostOp done = std::exchange(op, GhostOp::None);
        auto& slot = slots[active];
        const u64 at = block + active * SlotSize;
        switch (done) {
        case GhostOp::Create: {
            const auto handle = port.R64(at);
            if (!(x0 & 1) || !handle || !*handle) {
                Line(log, slot, fmt::format("create refused (w0={} handle={:#x})", x0 & 1, handle.value_or(0)));
                slot.state = State::None;
                slot.retry_tick = tick + (Fallback(log, slot) ? 30 : CreateRetry);
            } else {
                Line(log, slot, fmt::format("create requested name={} banc={}{}", slot_name, slot.banc,
                                            slot.adult_fallback ? " (adult peer: the profile has no adult "
                                                                  "puppet, drawn as the child one)"
                                                                : ""));
                slot.state = State::Pending;
                slot.pending_tick = tick;
                slot.pending_logged = false;
            }
            break;
        }
        case GhostOp::Get: {
            const auto vtable = x0 ? port.R64(x0) : std::nullopt;
            slot.fields.clear();
            bool ok = vtable && *vtable;
            for (const u64 f : g.actor_fields) {
                const auto v = ok ? port.R64(x0 + f) : std::nullopt;
                ok = ok && v && *v;
                slot.fields.push_back(v.value_or(0));
            }
            const auto record = ok ? port.R64(x0 + g.link[0]) : std::nullopt;
            const auto generation = ok ? port.R32(x0 + g.link[1]) : std::nullopt;
            ok = ok && record && *record && generation && port.R64(*record + g.link[2]) == x0 &&
                 port.R32(*record + g.link[3]) == *generation;
            if (!ok) {
                Line(log, slot, fmt::format("get returned no usable actor ({:#x})", x0));
                slot.state = State::None;
                slot.retry_tick = tick + CreateRetry;
                break;
            }
            slot.actor = x0;
            slot.vtable = *vtable;
            slot.record = *record;
            slot.generation = *generation;
            slot.state = State::Live;
            slot.parked = true; // the next visible frame moves it into place
            slot.played.clear();
            slot.play_logged = false;
            slot.alpha_done = false;
            slot.tint_done = false;
            slot.placed = false;
            slot.abandon = false;
            slot.watch_bad = slot.hide_bad = 0;
            slot.watch_tick = tick;
            slot.hide_seen.reset();
            Line(log, slot, fmt::format("live actor={:#x} generation={:#x}", x0, *generation));
            break;
        }
        case GhostOp::Release:
            slot.state = State::None;
            slot.retry_tick = tick + CreateRetry;
            Line(log, slot, "create released");
            break;
        case GhostOp::Alpha:
            Line(log, slot, fmt::format("alpha {}%", g.alpha_percent));
            break;
        case GhostOp::Tint: {
            const auto& c = g.tint_colors[active % g.tint_colors.size()];
            Line(log, slot, fmt::format("tint ({:.2f}, {:.2f}, {:.2f}, {:.2f}) set (C10: reaches the eyes only)",
                                        c[0], c[1], c[2], c[3]));
            break;
        }
        case GhostOp::Play:
            if (!(x0 & 1) || !slot.play_logged) {
                Line(log, slot, fmt::format("anim {} -> {} w0={}", slot.played, slot_name, x0 & 1));
                slot.play_logged = slot.play_logged || (x0 & 1);
            }
            break;
        case GhostOp::Move:
            if (slot.abandon) Abandoned(log, slot);
            break;
        case GhostOp::None:
            break;
        }
    }

    u64 Moves() const { return moves; }

private:
    static constexpr u64 TintAt = 0x1E0; ///< 16 bytes of RGBA in the slot

    std::optional<u64> Model(Port& port, const Slot& slot) {
        if (!g.alpha_chain[0]) return {};
        const auto holder = port.R64(slot.actor + g.alpha_chain[0]);
        const auto model = holder && *holder ? port.R64(*holder + g.alpha_chain[1]) : std::nullopt;
        return model && *model ? model : std::nullopt;
    }
    std::optional<std::array<float, 3>> ActorPos(Port& port, const Slot& slot) {
        if (!g.watch_pos) return {};
        std::array<float, 3> p{};
        for (size_t k = 0; k < 3; ++k) {
            const auto bits = port.R32(slot.actor + g.watch_pos + k * 4);
            if (!bits) return {};
            p[k] = GhostDetail::Float(*bits);
            if (!std::isfinite(p[k])) return {};
        }
        return p;
    }
    std::optional<u32> HideByte(Port& port, const Slot& slot) {
        if (!g.watch_hide) return {};
        const auto model = Model(port, slot);
        const auto word = model ? port.R32(*model + (g.watch_hide & ~u64{3})) : std::nullopt;
        if (!word) return {};
        return (*word >> ((g.watch_hide & 3) * 8)) & 0xFF;
    }
    /// Pure reads on a validated live actor: does it still follow set-transform, is the game
    /// hiding it while we show it? Three strikes in a row abandon it (park, then re-create).
    template <typename Log>
    void Watch(Port& port, Slot& slot, Log log) {
        if (slot.abandon || slot.abandons >= AbandonLimit || tick - slot.watch_tick < WatchEvery) return;
        slot.watch_tick = tick;
        if (const auto hide = HideByte(port, slot);
            hide && (slot.hide_seen ? *slot.hide_seen != *hide : *hide != 0)) {
            Line(log, slot, fmt::format("hide byte {:#04x} -> {:#04x}{}", slot.hide_seen.value_or(0), *hide,
                                        slot.parked ? " (parked)" : ""));
            slot.hide_seen = *hide;
        } else if (hide) {
            slot.hide_seen = *hide;
        }
        if (slot.parked || !slot.placed) {
            slot.watch_bad = slot.hide_bad = 0;
            return;
        }
        if (const auto p = ActorPos(port, slot)) {
            const float dx = (*p)[0] - slot.drawn[0], dy = (*p)[1] - slot.drawn[1], dz = (*p)[2] - slot.drawn[2];
            slot.watch_bad = std::sqrt(dx * dx + dy * dy + dz * dz) > WatchFar ? slot.watch_bad + 1 : 0;
            if (slot.watch_bad >= WatchStrikes) {
                Abandon(log, slot, fmt::format("actor at ({:.2f}, {:.2f}, {:.2f}) but drawn at ({:.2f}, {:.2f}, "
                                               "{:.2f}): it stopped following set-transform",
                                               (*p)[0], (*p)[1], (*p)[2], slot.drawn[0], slot.drawn[1],
                                               slot.drawn[2]));
                return;
            }
        }
        if (const auto hide = HideByte(port, slot)) {
            slot.hide_bad = *hide ? slot.hide_bad + 1 : 0;
            if (slot.hide_bad >= WatchStrikes)
                Abandon(log, slot, fmt::format("the game hides it (hide byte {:#04x}) while it should show", *hide));
        }
    }
    template <typename Log>
    void Abandon(Log log, Slot& slot, std::string_view why) {
        ++slot.abandons;
        Line(log, slot, fmt::format("watchdog: {}; parking it and re-creating ({} of {})", why, slot.abandons,
                                    AbandonLimit));
        slot.abandon = true;
    }
    template <typename Log>
    void Abandoned(Log log, Slot& slot) {
        Line(log, slot, "abandoned (parked); re-creating");
        slot.state = State::None;
        slot.actor = 0;
        slot.abandon = false;
        slot.retry_tick = tick + 30;
    }
    template <typename Log>
    void Status(Port& port, const Pose& local, const std::vector<PeerView>& peers, Log log) {
        for (auto& slot : slots) {
            if (slot.origin.empty() || status_lines >= 2000) continue;
            ++status_lines;
            const PeerView* peer = nullptr;
            for (const auto& p : peers) if (p.origin == slot.origin) peer = &p;
            std::string peer_text = !peer ? "gone" : !peer->pose ? "stale" : "";
            if (peer && peer->pose) {
                const auto& p = *peer->pose;
                const float dx = p.pos[0] - local.pos[0], dz = p.pos[2] - local.pos[2];
                peer_text = fmt::format("{}{} ({:.2f}, {:.2f}, {:.2f}) {} dist={:.1f}", p.banc.empty() ? "-" : p.banc,
                                        p.loading ? " loading" : "", p.pos[0], p.pos[1], p.pos[2], p.anim,
                                        std::sqrt(dx * dx + dz * dz));
            }
            std::string actor_text;
            if (slot.state == State::Live) {
                const auto p = ActorPos(port, slot);
                const auto hide = HideByte(port, slot);
                const auto life = g.lifecycle ? port.R32(slot.actor + g.lifecycle) : std::nullopt;
                actor_text = fmt::format(" drawn=({:.2f}, {:.2f}, {:.2f}){}{}{}{}", slot.drawn[0], slot.drawn[1],
                                         slot.drawn[2],
                                         p ? fmt::format(" actor=({:.2f}, {:.2f}, {:.2f})", (*p)[0], (*p)[1], (*p)[2])
                                           : std::string{},
                                         hide ? fmt::format(" hide={:#04x}", *hide) : std::string{},
                                         life ? fmt::format(" lifecycle={}", *life) : std::string{},
                                         slot.parked ? " parked" : " shown");
            }
            Line(log, slot, fmt::format("status peer={} local={}{} state={}{}", peer_text,
                                        local.banc.empty() ? "-" : local.banc, local.loading ? " loading" : "",
                                        slot.state == State::Live ? "live" : slot.state == State::Pending ? "pending"
                                                                                                         : "none",
                                        actor_text),
                 true);
        }
    }
    Slot* Find(std::string_view origin) {
        for (auto& slot : slots) if (slot.origin == origin) return &slot;
        return nullptr;
    }
    /// The take consumed the request handle; from then on the actor is tracked like the game's own
    /// actor link (record still names it with the same generation, lifecycle not in the teardown
    /// range), plus vtable and fields. Nothing when it is still alive, else why not.
    std::optional<std::string> Dead(Port& port, const Slot& slot) {
        if (port.R64(slot.actor + g.link[0]) != slot.record ||
            port.R64(slot.record + g.link[2]) != slot.actor ||
            port.R32(slot.record + g.link[3]) != slot.generation || port.R64(slot.actor) != slot.vtable)
            return "actor no longer valid";
        for (size_t i = 0; i < g.actor_fields.size(); ++i)
            if (port.R64(slot.actor + g.actor_fields[i]) != slot.fields[i]) return "actor no longer valid";
        if (g.lifecycle) {
            const auto life = port.R32(slot.actor + g.lifecycle);
            if (!life) return "actor no longer valid";
            if (*life >= g.lifecycle_lo && *life <= g.lifecycle_hi)
                return fmt::format("actor being deleted, lifecycle {}", *life);
        }
        return std::nullopt;
    }
    /// Switch this slot to the fallback puppet once; false when there is none or it is in use.
    template <typename Log>
    bool Fallback(Log log, Slot& slot) {
        if (slot.fallback || g.actor_fallback.empty()) return false;
        slot.fallback = true;
        Line(log, slot, fmt::format("falling back to {}", g.actor_fallback));
        return true;
    }
    template <typename Log>
    void Drop(Log log, Slot& slot, std::string_view why) {
        Line(log, slot, fmt::format("dropped ({})", why));
        slot.state = State::None;
        slot.actor = 0;
        slot.retry_tick = tick + 60;
    }
    bool Start(Port& port, const OotGameDataTrace::Entry& entry, std::array<u64, 8> args, size_t i, GhostOp what,
               std::optional<float> s0 = std::nullopt) {
        const auto address = port.Verified(entry);
        if (!address || !port.Call(*address, args, s0)) return false;
        return Started(i, what);
    }
    bool Started(size_t i, GhostOp what) {
        op = what;
        active = i;
        next = (i + 1) % GhostSlots;
        return true;
    }
    template <typename Log>
    bool StartCreate(Port& port, u64 at, Slot& slot, const Pose& peer, const std::string& banc, Log log) {
        const auto manager = port.R64(base + g.actor_mgr_cell);
        const auto entry = port.Verified(g.create);
        std::array<u8, 12> scale{};
        for (size_t i = 0; i < 3; ++i) {
            const auto w = port.R32(base + g.scale_const + i * 4);
            if (!w) return false;
            std::memcpy(scale.data() + i * 4, &*w, 4);
        }
        if (!manager || !*manager || !entry) return false;
        // No adult puppet in the profile: an adult peer is drawn with the child one (logged once
        // per create, with the create line).
        const std::string& name = slot.fallback ? g.actor_fallback
                                  : peer.child || g.actor_adult.empty() ? g.actor_child : g.actor_adult;
        slot.adult_fallback = !slot.fallback && !peer.child && g.actor_adult.empty();
        // Slot layout: +0 handle, +8 name pointer, +0x10 matrix (0x30), +0x40 create argument
        // (0xA0), +0xE0 empty instance name, +0x100 actor name (64 bytes).
        std::array<u8, 0x140> b{};
        const u64 name_at = at + 0x100, empty_at = at + 0xE0;
        std::memcpy(b.data() + 8, &name_at, 8);
        const auto m = YawMatrix(peer.pos, peer.yaw);
        const std::array<float, 12> arg_head{peer.pos[0], peer.pos[1], peer.pos[2], m[0], m[1], m[2],
                                             m[4], m[5], m[6], m[8], m[9], m[10]};
        std::memcpy(b.data() + 0x40, arg_head.data(), sizeof(arg_head)); // position, 3x3 rotation
        std::memcpy(b.data() + 0x40 + 0x30, scale.data(), scale.size());
        std::memcpy(b.data() + 0x40 + 0x40, &empty_at, 8);
        b[0x40 + 0x98] = 3;
        std::memcpy(b.data() + 0x100, name.data(), std::min<size_t>(name.size(), 63));
        if (!port.Write(at, b.data(), b.size())) return false;
        // x0 manager, x1 &namePtr, x2 &argument, x3 &handle, w4 1; three zero stack arguments.
        if (!port.Call(*entry, {*manager, at + 8, at + 0x40, at, 1, 0, 0, 0})) return false;
        slot.banc = banc;
        slot_name = name;
        (void)log;
        return true;
    }
    template <typename Log>
    void Line(Log log, const Slot& slot, std::string_view what, bool status = false) {
        if (!status) {
            if (lines >= LineLimit) return;
            if (++lines == LineLimit) {
                log("OoT co-op: ghost log cap reached; later transitions are not logged");
                return;
            }
        }
        log(fmt::format("OoT co-op: ghost {} {}", slot.origin.substr(0, 8), what));
    }
    static constexpr u64 LineLimit = 4096;

    const OotGameDataTrace::GhostOffsets& g;
    const u64 base;
    std::array<Slot, GhostSlots> slots{};
    GhostOp op{GhostOp::None};
    size_t active{}, next{};
    u64 tick{}, moves{}, lines{}, status_lines{};
    std::string slot_name;
};

} // namespace Core::OotCoop
