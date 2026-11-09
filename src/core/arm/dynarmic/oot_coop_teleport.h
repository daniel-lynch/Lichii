// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <deque>
#include <functional>
#include <map>
#include "core/arm/dynarmic/oot_coop_anim.h"
#include "core/arm/dynarmic/oot_coop_apply.h"
#include "core/arm/dynarmic/oot_coop_ghost.h"

namespace Core::OotCoop::Teleport {
using Vec = std::array<float, 3>;
using Log = std::function<void(std::string_view)>;
struct FallOrigin {
    std::vector<u64> chain;
    u64 owner{}, field{};
    u16 bit{}; // mask, exactly one bit
};
struct Keys {
    bool enabled{};
    u64 actor_pos{}, event_active{};
    std::vector<u32> block_bools;
    std::vector<std::string> requester_commands, answer_commands, allow_bancs, deny_bancs;
    u32 settle_frames{}, verify_frames{}, reply_timeout_ms{}, apply_timeout_ms{};
    float settle_epsilon{}, agree_epsilon{};
    std::array<float, 2> tolerance{};
    std::optional<FallOrigin> fall_origin;
};
[[nodiscard]] std::optional<Keys> DecodeKeys(const nlohmann::json& value);
[[nodiscard]] bool Reason(std::string_view reason);
struct Packet {
    bool request{}, ok{}, child{};
    std::string peer, banc, reason;
    u64 id{};
    std::optional<u8> room;
    Vec pos{};
    float yaw{};
};
[[nodiscard]] std::optional<Packet> DecodePacket(const nlohmann::json& value, bool server = true);
[[nodiscard]] nlohmann::json EncodePacket(const Packet& packet, bool server = false);
struct Sample {
    std::string banc;
    bool child{}, loading{true};
    std::optional<u64> event_running; // pointer value for the receipt, zero means clear
    std::optional<bool> block_bools_ok;
    std::optional<std::string> command;
    Vec info_pos{};
    std::optional<Vec> actor_pos;
    float yaw{};
    std::optional<Player::Resident> resident;
};
struct History {
    std::optional<Sample> previous;
    Vec anchor{};
    u32 frames{};
    void Step(const Sample& sample, const Keys& keys);
    void Reset() { previous.reset(); frames = 0; }
};
[[nodiscard]] std::string Ready(const Sample& sample, const History& history, const Keys& keys, bool answer);
[[nodiscard]] bool Within(const Vec& a, const Vec& b, const std::array<float, 2>& tolerance);
[[nodiscard]] ApplyTarget MatrixCall(u64 entry, u64 sentinel, u64 sp, u64 actor, const Packet& packet);

// Every memory read is made at the frame drain, independent of PoseSender's cached output.
template <typename Memory>
Sample ReadSample(Memory& memory, u64 base, u64 manager, const OotGameDataTrace::Profile& profile) {
    const auto& keys = *profile.teleport;
    const auto pose = ReadLocalPose(memory, base, manager, profile.ghost, 0);
    Sample s;
    s.banc = pose.banc; s.child = pose.child; s.loading = pose.loading;
    s.info_pos = pose.pos; s.yaw = pose.yaw;
    using Player::Detail::Read;
    const auto cell = Read(memory, base, profile.event_got, 8);
    const auto events = cell ? Read(memory, *cell, 0, 8) : std::nullopt;
    if (events) s.event_running = Read(memory, *events, keys.event_active, 8);
    s.block_bools_ok = true;
    for (u32 hash : keys.block_bools) {
        const auto flag = manager ? GhostDetail::Flag(memory, manager, hash) : std::nullopt;
        if (!flag) { s.block_bools_ok.reset(); break; }
        if (*flag) s.block_bools_ok = false;
    }
    const auto player = Anim::FindPlayer(memory, base, s.child, *profile.anim);
    if (!player) return s;
    s.resident = Player::Resident{player->actor, player->record, player->equipment, player->generation};
    Vec pos{};
    bool valid = true;
    for (size_t i = 0; i < 3; ++i) {
        const auto bits = Read(memory, player->actor, keys.actor_pos + i * 4, 4);
        if (!bits) { valid = false; break; }
        pos[i] = std::bit_cast<float>(static_cast<u32>(*bits));
        if (!std::isfinite(pos[i]) || std::abs(pos[i]) > PoseCoordinateLimit) valid = false;
    }
    const auto snap = Anim::ReadSnapshot(memory, player->actor, s.child, *profile.anim,
        [&] { return Anim::ValidatePlayer(memory, base, *player, *profile.anim); });
    if (snap && profile.anim->source_slot < snap->controllers.size())
        s.command = snap->controllers[profile.anim->source_slot].name;
    if (valid && Player::Validate(memory, base, *s.resident, *profile.anim->player)) s.actor_pos = pos;
    else { s.resident.reset(); s.command.reset(); }
    return s;
}

// The same private call applier as GhostPort, with a separate saved context owned by teleport.
// No actor transform stores are made here. The optional u16 OR mirrors the native one-shot.
template <typename Memory, typename Backend>
bool Issue(Memory& memory, Backend& backend, const OotGameDataTrace::Profile& profile, u64 base,
           u64 stack_top, u64 sentinel, const Player::Resident& resident, const Packet& destination,
           std::optional<typename Backend::Context>& saved_context, u64& call_stack) {
    const auto entry = backend.VerifiedEntry(profile.ghost.set_mtx);
    if (!entry || saved_context || stack_top < 0x100 || !backend.CallReady()) return false;
    auto saved = backend.Capture();
    if (!saved) return false;
    const u64 sp = stack_top - 0x100;
    const auto target = MatrixCall(*entry, sentinel, sp, resident.actor, destination);
    std::optional<std::pair<u64, u16>> fall;
    if (const auto& f = profile.teleport->fall_origin) {
        u64 component = resident.actor;
        for (u64 offset : f->chain) {
            const auto next = Player::Detail::Read(memory, component, offset, 8);
            if (!next || !*next) return false;
            component = *next;
        }
        if (Player::Detail::Read(memory, component, f->owner, 8) != resident.actor ||
            component > std::numeric_limits<u64>::max() - f->field - 2 ||
            !memory.IsValidVirtualAddressRange(component + f->field, 2)) return false;
        fall = {{component + f->field, memory.Read16(component + f->field)}};
    }
    if (!Player::Validate(memory, base, resident, *profile.anim->player)) return false;
    if (fall) memory.Write16(fall->first, fall->second | profile.teleport->fall_origin->bit);
    if (!backend.Redirect(target)) {
        if (fall) memory.Write16(fall->first, fall->second);
        return false;
    }
    saved_context = std::move(saved);
    call_stack = sp;
    return true;
}

struct Port {
    std::function<bool(const Player::Resident&)> validate;
    // Performs entry verification, full context capture, optional fall-origin OR and redirect.
    std::function<bool(const Player::Resident&, const Packet&)> call;
    Log log;
};
struct Peer { std::string id, name; };
[[nodiscard]] std::optional<std::string> ResolveTrigger(std::string_view line, const std::vector<Peer>& peers);
class TriggerFile {
public:
    explicit TriggerFile(std::string path_ = {});
    std::vector<std::string> Poll(u64 now);
private:
    std::string path, partial;
    u64 offset{}, last_poll{};
    bool overlong{};
};

// All methods are serialized by the session mutex; only Frame/Returned use guest state.
class Controller {
public:
    explicit Controller(Keys keys_) : keys{std::move(keys_)} {}
    bool Trigger(std::string target, u64 now, const Log& log);
    void Receive(const Packet& packet, u64 now, const Log& log);
    void Tick(u64 now, const Log& log);
    std::deque<Packet> TakeOutput() { return std::exchange(output, {}); }
    bool Frame(const Sample& sample, u64 now, bool can_call, Port& port);
    void Returned(u64 result, const Log& log);
    void Invalidate(const Log& log, std::string_view why, bool disconnected = false);
    void PeerGone(std::string_view peer, const Log& log);
    [[nodiscard]] bool Calling() const { return phase == Phase::Calling; }
    [[nodiscard]] bool Busy() const { return phase != Phase::Idle; }
private:
    enum class Phase { Idle, Waiting, Destination, Calling, Verify };
    void Finish(const Log& log, std::string_view why);
    Keys keys;
    History history;
    Phase phase{Phase::Idle};
    Packet destination;
    u64 next_id{1}, started{}, arrived_at{}, frame{}, issued_frame{}, verify_start{};
    std::optional<Player::Resident> resident;
    std::optional<Sample> source;
    std::string scene;
    bool child{}, abandoned{};
    struct Incoming { Packet packet; u64 at{}; };
    std::deque<Incoming> requests;
    std::map<std::string, u64, std::less<>> seen;
    std::deque<Packet> output;
    struct Checkpoint { Packet dest; Player::Resident actor; u64 frame{}; unsigned next{}; };
    std::optional<Checkpoint> checkpoint;
};
} // namespace Core::OotCoop::Teleport
