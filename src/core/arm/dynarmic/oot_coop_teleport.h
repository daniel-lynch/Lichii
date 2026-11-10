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
struct CrossKeys {
    bool enabled{}, explicit_position{};
    OotGameDataTrace::Entry string64, enumeration, vector3, request_ctor, request_event;
    u32 request_size{}, request_name_ptr{}, request_name_cap{};
    u32 reserved_banc{}, reserved_start{}, reserved_position{}, fade_key{}, fade_value{}, control_key{}, control_value{};
    std::vector<std::string> events;
    std::map<std::string, std::string, std::less<>> start_pos;
    u32 load_timeout_ms{}, arrive_frames{};
};
enum class CrossStep { Banc, Start, Position, Fade, Control, Constructor, Event, Reassert };
struct Keys {
    bool enabled{};
    u64 actor_pos{}, event_active{};
    std::vector<u32> block_bools;
    std::vector<std::string> requester_commands, answer_commands, allow_bancs, deny_bancs;
    u32 settle_frames{}, verify_frames{}, reply_timeout_ms{}, apply_timeout_ms{};
    float settle_epsilon{}, agree_epsilon{};
    std::array<float, 2> tolerance{};
    std::optional<FallOrigin> fall_origin;
    std::optional<CrossKeys> cross;
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
[[nodiscard]] std::string Ready(const Sample& sample, const History& history, const Keys& keys, bool answer,
                                const std::vector<std::string>& exempt_bancs = {});
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

// Entry verification and full saved context are shared by matrix and transition calls.
template <typename Backend, typename Prepare>
bool IssueCall(Backend& backend, const OotGameDataTrace::Entry& entry, u64 stack_top,
               u64 sentinel, std::optional<typename Backend::Context>& saved_context,
               u64& call_stack, Prepare prepare) {
    if (saved_context || stack_top < 0x100 || !backend.CallReady()) return false;
    const auto address = backend.VerifiedEntry(entry);
    if (!address) return false;
    auto saved = backend.Capture();
    if (!saved) return false;
    const u64 sp = stack_top - 0x100;
    ApplyTarget target{true, false, 0, 0, *address, sentinel, sp};
    target.stores = {{sp, 0}, {sp + 8, 0}, {sp + 16, 0}, {sp + 24, 0}};
    if (!prepare(target) || !backend.Redirect(target)) return false;
    saved_context = std::move(saved);
    call_stack = sp;
    return true;
}

// Layout relative to the bottom of the existing private 64 KiB stack, not game offsets.
inline constexpr u64 ScratchSize = 0x2000, RequestLimit = 0x1C00;
inline constexpr u64 BancText = RequestLimit, StartText = BancText + 0x80;
inline constexpr u64 BancCell = StartText + 0x80, StartCell = BancCell + 8, PositionData = StartCell + 8;
template <typename Memory, typename Backend>
bool IssueCross(Memory& memory, Backend& backend, const OotGameDataTrace::Profile& profile,
                u64 base, u64 stack_top, u64 sentinel, CrossStep step, size_t event,
                const Packet& destination, std::optional<typename Backend::Context>& saved_context,
                u64& call_stack) {
    if (!profile.teleport || !profile.teleport->cross || saved_context || stack_top < 0x10000) return false;
    const auto& k = *profile.teleport->cross;
    const u64 scratch = stack_top - 0x10000;
    if (!k.enabled || !memory.IsValidVirtualAddressRange(scratch, ScratchSize)) return false;
    if (event >= k.events.size()) return false;
    const auto* entry = &k.enumeration;
    if (step == CrossStep::Banc || step == CrossStep::Start) entry = &k.string64;
    if (step == CrossStep::Position) entry = &k.vector3;
    if (step == CrossStep::Constructor) entry = &k.request_ctor;
    if (step == CrossStep::Event) entry = &k.request_event;
    return IssueCall(backend, *entry, stack_top, sentinel, saved_context, call_stack, [&](ApplyTarget& t) {
        using Player::Detail::Read;
        const auto manager = Read(memory, base, profile.manager_cell, 8);
        const auto cell = Read(memory, base, profile.event_got, 8);
        const auto events = cell ? Read(memory, *cell, 0, 8) : std::nullopt;
        if (!manager || !*manager || !events || !*events) return false;
        auto text = [&](u64 at, std::string_view value) {
            for (size_t i = 0; i < value.size(); ++i) memory.Write8(at + i, static_cast<u8>(value[i]));
            memory.Write8(at + value.size(), 0);
        };
        t.arguments = {*manager, 0, 0};
        switch (step) {
        case CrossStep::Banc:
            if (destination.banc.size() >= 64) return false;
            text(scratch + BancText, destination.banc);
            memory.Write64(scratch + BancCell, scratch + BancText);
            t.arguments[1] = scratch + BancCell; t.arguments[2] = k.reserved_banc;
            break;
        case CrossStep::Start: {
            const auto it = k.start_pos.find(destination.banc);
            if (!k.explicit_position && it == k.start_pos.end()) return false;
            const std::string name = k.explicit_position ? "" : it->second;
            if (name.size() >= 64) return false;
            text(scratch + StartText, name);
            memory.Write64(scratch + StartCell, scratch + StartText);
            t.arguments[1] = scratch + StartCell; t.arguments[2] = k.reserved_start;
            break;
        }
        case CrossStep::Position:
            if (!k.explicit_position) return false;
            for (size_t i = 0; i < 3; ++i) memory.Write32(scratch + PositionData + i * 4, std::bit_cast<u32>(destination.pos[i]));
            t.arguments[1] = scratch + PositionData; t.arguments[2] = k.reserved_position;
            break;
        case CrossStep::Fade:
            t.arguments[1] = k.fade_value; t.arguments[2] = k.fade_key;
            break;
        case CrossStep::Control: case CrossStep::Reassert:
            t.arguments[1] = k.control_value; t.arguments[2] = k.control_key;
            break;
        case CrossStep::Constructor:
            t.arguments = {scratch};
            break;
        case CrossStep::Event: {
            const u64 pointer = memory.Read64(scratch + k.request_name_ptr);
            const u32 capacity = memory.Read32(scratch + k.request_name_cap);
            const auto& name = k.events[event];
            if (pointer < scratch || pointer - scratch >= k.request_size ||
                capacity < name.size() + 1 || capacity > k.request_size - (pointer - scratch)) return false;
            // Do not allow a corrupt SafeString to overwrite its own descriptor.
            const auto overlaps = [&](u64 at, u64 n) { return pointer < at + n && at < pointer + name.size() + 1; };
            if (overlaps(scratch + k.request_name_ptr, 8) || overlaps(scratch + k.request_name_cap, 4)) return false;
            text(pointer, name);
            t.arguments = {*events, scratch};
            break;
        }
        }
        return true;
    });
}

// The same private call applier as GhostPort, with a separate saved context owned by teleport.
// No actor transform stores are made here. The optional u16 OR mirrors the native one-shot.
template <typename Memory, typename Backend>
bool Issue(Memory& memory, Backend& backend, const OotGameDataTrace::Profile& profile, u64 base,
           u64 stack_top, u64 sentinel, const Player::Resident& resident, const Packet& destination,
           std::optional<typename Backend::Context>& saved_context, u64& call_stack) {
    if (saved_context) return false;
    std::optional<std::pair<u64, u16>> fall;
    bool fall_written = false;
    const bool issued = IssueCall(backend, profile.ghost.set_mtx, stack_top, sentinel, saved_context, call_stack,
        [&](ApplyTarget& target) {
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
            target = MatrixCall(target.entry, sentinel, target.stack, resident.actor, destination);
            if (fall) {
                memory.Write16(fall->first, fall->second | profile.teleport->fall_origin->bit);
                fall_written = true;
            }
            return true;
        });
    if (!issued && fall_written) memory.Write16(fall->first, fall->second);
    return issued;
}

struct Port {
    std::function<bool(const Player::Resident&)> validate;
    // Performs entry verification, full context capture, optional fall-origin OR and redirect.
    std::function<bool(const Player::Resident&, const Packet&)> call;
    Log log;
    std::function<bool(CrossStep, size_t, const Packet&)> cross_call;
    std::function<void()> arrived;
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
    void SetExemptBancs(std::vector<std::string> bancs) { exempt_bancs = std::move(bancs); }
    bool Trigger(std::string target, u64 now, const Log& log);
    void Receive(const Packet& packet, u64 now, const Log& log);
    void Tick(u64 now, const Log& log);
    std::deque<Packet> TakeOutput() { return std::exchange(output, {}); }
    bool Frame(const Sample& sample, u64 now, bool can_call, Port& port);
    void Returned(u64 result, const Log& log);
    void Invalidate(const Log& log, std::string_view why, bool disconnected = false);
    void PeerGone(std::string_view peer, const Log& log);
    [[nodiscard]] bool Calling() const { return phase == Phase::Calling || phase == Phase::CrossCalling; }
    [[nodiscard]] bool CrossActive() const { return cross_active && Busy(); }
    [[nodiscard]] bool TransitionWait() const { return phase == Phase::Load; }
    // A table relocation with the same manager can be an expected load, not a save identity change.
    bool Rebinding(const Sample& sample, u64 now, bool same_manager, const Log& log);
    [[nodiscard]] bool Busy() const { return phase != Phase::Idle; }
private:
    enum class Phase { Idle, Waiting, Destination, Calling, Verify, Reserve, CrossCalling, Load };
    void Finish(const Log& log, std::string_view why);
    bool CrossFrame(const Sample& sample, u64 now, bool can_call, Port& port);
    bool CrossIssue(Port& port);
    void CrossFail(const Log& log, std::string_view why);
    bool cross_active{}, event_live{}, reasserted{}, loading_on{}, switched{}, loading_off{}, old_gone{};
    CrossStep cross_step{CrossStep::Banc};
    size_t event_index{};
    u64 cross_started{};
    u32 arrival_frames{};
    std::optional<Player::Resident> new_resident;
    Vec cross_from{}; ///< PlayingInfo position when the cross teleport started
    Keys keys;
    std::vector<std::string> exempt_bancs;
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
