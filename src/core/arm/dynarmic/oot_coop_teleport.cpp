// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/arm/dynarmic/oot_coop_teleport.h"

#include <charconv>
#include <fstream>
#include <filesystem>
#include <fmt/format.h>

namespace Core::OotCoop::Teleport {
namespace {
using Json = nlohmann::json;
constexpr u64 MaxId = (u64{1} << 53) - 1;
std::optional<u64> Number(const Json& v, u64 hi, bool strings = true) {
    u64 n{};
    if (v.is_number_unsigned()) n = v.get<u64>();
    else if (v.is_number_integer()) {
        const auto i = v.get<s64>();
        if (i < 0) return {};
        n = static_cast<u64>(i);
    } else if (strings && v.is_string()) {
        const auto s = v.get<std::string>();
        const bool hex = s.starts_with("0x") || s.starts_with("0X");
        const char* b = s.data() + (hex ? 2 : 0);
        const auto [p, ec] = std::from_chars(b, s.data() + s.size(), n, hex ? 16 : 10);
        if (ec != std::errc{} || p != s.data() + s.size() || p == b) return {};
    } else return {};
    return n <= hi ? std::optional<u64>{n} : std::nullopt;
}
bool Real(const Json& v, double limit) {
    return v.is_number() && std::isfinite(v.get<double>()) && std::abs(v.get<double>()) <= limit;
}
bool Id(std::string_view s) {
    return !s.empty() && s.size() <= 64 && std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '.' || c == '-';
    });
}
bool Contains(const std::vector<std::string>& list, std::string_view s) {
    return std::find(list.begin(), list.end(), s) != list.end();
}
bool Position(const Vec& p) {
    return std::all_of(p.begin(), p.end(), [](float f) { return std::isfinite(f) && std::abs(f) <= 100000; });
}
std::string Details(const Sample& s) {
    return fmt::format("banc={} child={} loading={} event={} blocks={} command={} info=({},{},{}) actor=({},{},{})",
        s.banc, s.child, s.loading, s.event_running ? fmt::format("{:#x}", *s.event_running) : "unreadable",
        s.block_bools_ok ? (*s.block_bools_ok ? "clear" : "blocked") : "unreadable",
        s.command.value_or("unreadable"), s.info_pos[0], s.info_pos[1], s.info_pos[2],
        s.actor_pos ? (*s.actor_pos)[0] : NAN, s.actor_pos ? (*s.actor_pos)[1] : NAN,
        s.actor_pos ? (*s.actor_pos)[2] : NAN);
}
} // namespace

std::optional<Keys> DecodeKeys(const Json& v) {
    try {
        if (!v.is_object() ||
            v.size() != size_t{15} + (v.contains("fall_origin") ? 1U : 0U) + (v.contains("cross") ? 1U : 0U))
            return {};
        Keys k;
        if (!v.at("enabled").is_boolean()) return {};
        k.enabled = v.at("enabled").get<bool>();
        for (auto [name, member] : {std::pair{"actor_pos", &Keys::actor_pos}, {"event_active", &Keys::event_active}}) {
            const auto n = Number(v.at(name), 0x100000);
            if (!n || (*n & 3)) return {};
            k.*member = *n;
        }
        const auto& hashes = v.at("block_bools");
        if (!hashes.is_array() || hashes.empty() || hashes.size() > 32) return {};
        for (const auto& hash : hashes) {
            const auto n = Number(hash, 0xFFFFFFFF);
            if (!n || !*n) return {};
            k.block_bools.push_back(static_cast<u32>(*n));
        }
        for (auto [name, member] : {std::pair{"requester_commands", &Keys::requester_commands},
                {"answer_commands", &Keys::answer_commands}, {"allow_bancs", &Keys::allow_bancs}, {"deny_bancs", &Keys::deny_bancs}}) {
            const auto& list = v.at(name);
            if (!list.is_array() || list.size() > 128 || (list.empty() && member != &Keys::deny_bancs)) return {};
            for (const auto& item : list) {
                if (!item.is_string()) return {};
                const auto s = item.get<std::string>();
                if (s.empty() || !BancName(s) || Contains(k.*member, s)) return {};
                (k.*member).push_back(s);
            }
        }
        for (auto [name, member] : {std::pair{"settle_frames", &Keys::settle_frames}, {"verify_frames", &Keys::verify_frames},
                {"reply_timeout_ms", &Keys::reply_timeout_ms}, {"apply_timeout_ms", &Keys::apply_timeout_ms}}) {
            const auto n = Number(v.at(name), member == &Keys::settle_frames || member == &Keys::verify_frames ? 600 : 10000);
            if (!n || !*n) return {};
            k.*member = static_cast<u32>(*n);
        }
        for (auto [name, member] : {std::pair{"settle_epsilon", &Keys::settle_epsilon}, {"agree_epsilon", &Keys::agree_epsilon}}) {
            if (!Real(v.at(name), 10) || v.at(name).get<double>() <= 0) return {};
            k.*member = v.at(name).get<float>();
            if (k.*member <= 0) return {};
        }
        const auto& t = v.at("tolerance");
        if (!t.is_array() || t.size() != 2) return {};
        for (size_t i = 0; i < 2; ++i) {
            if (!Real(t[i], 10) || t[i].get<double>() <= 0) return {};
            k.tolerance[i] = t[i].get<float>();
            if (k.tolerance[i] <= 0) return {};
        }
        if (v.contains("fall_origin")) {
            const auto& f = v.at("fall_origin");
            if (!f.is_object() || f.size() != 4 || !f.at("chain").is_array() || f.at("chain").empty() || f.at("chain").size() > 8) return {};
            FallOrigin o;
            for (const auto& x : f.at("chain")) {
                const auto n = Number(x, 0x100000);
                if (!n || (*n & 7)) return {};
                o.chain.push_back(*n);
            }
            const auto owner = Number(f.at("owner"), 0x100000), field = Number(f.at("field"), 0x100000), bit = Number(f.at("bit"), 0xFFFF);
            if (!owner || (*owner & 7) || !field || (*field & 1) || !bit || !*bit || (*bit & (*bit - 1))) return {};
            o.owner = *owner; o.field = *field; o.bit = static_cast<u16>(*bit);
            k.fall_origin = o;
        }
        if (v.contains("cross")) {
            const auto& c = v.at("cross");
            if (!c.is_object() || !c.at("enabled").is_boolean() || !c.at("mode").is_string()) return {};
            CrossKeys x;
            x.enabled = c.at("enabled").get<bool>();
            const auto mode = c.at("mode").get<std::string>();
            if (mode != "startpos" && mode != "explicit") return {};
            x.explicit_position = mode == "explicit";
            if (c.size() != (x.explicit_position ? 21 : 19)) return {};
            for (auto [name, member] : {std::pair{"string64", &CrossKeys::string64}, {"enum", &CrossKeys::enumeration},
                    {"request_ctor", &CrossKeys::request_ctor}, {"request_event", &CrossKeys::request_event},
                    {"vector3", &CrossKeys::vector3}}) {
                if (member == &CrossKeys::vector3 && !x.explicit_position) continue;
                const auto& e = c.at(name);
                if (!e.is_object() || e.size() != 2 || !e.at("words").is_array() || e.at("words").size() != 4) return {};
                const auto at = Number(e.at("at"), 0x10000000);
                if (!at || !*at || (*at & 3)) return {};
                (x.*member).offset = *at;
                for (const auto& word : e.at("words")) {
                    const auto n = Number(word, UINT32_MAX);
                    if (!n) return {};
                    (x.*member).words.push_back(static_cast<u32>(*n));
                }
            }
            for (auto [name, member] : {std::pair{"request_size", &CrossKeys::request_size},
                    {"request_name_ptr", &CrossKeys::request_name_ptr}, {"request_name_cap", &CrossKeys::request_name_cap},
                    {"reserved_banc", &CrossKeys::reserved_banc}, {"reserved_start", &CrossKeys::reserved_start},
                    {"reserved_position", &CrossKeys::reserved_position}, {"fade_key", &CrossKeys::fade_key},
                    {"fade_value", &CrossKeys::fade_value}, {"control_key", &CrossKeys::control_key},
                    {"control_value", &CrossKeys::control_value}, {"load_timeout_ms", &CrossKeys::load_timeout_ms},
                    {"arrive_frames", &CrossKeys::arrive_frames}}) {
                if (member == &CrossKeys::reserved_position && !x.explicit_position) continue;
                const auto n = Number(c.at(name), UINT32_MAX);
                if (!n || !*n) return {};
                x.*member = static_cast<u32>(*n);
            }
            if (x.request_size > RequestLimit || x.request_size < 16 || (x.request_size & 15) ||
                (x.request_name_ptr & 7) || x.request_name_ptr > x.request_size - 8 ||
                (x.request_name_cap & 3) || x.request_name_cap > x.request_size - 4 ||
                (x.request_name_ptr < x.request_name_cap + 4 && x.request_name_cap < x.request_name_ptr + 8) ||
                x.load_timeout_ms < 1000 || x.load_timeout_ms > 120000 || x.arrive_frames > 600) return {};
            const auto& events = c.at("events");
            if (!events.is_array() || events.empty() || events.size() > 4) return {};
            for (const auto& event : events) {
                if (!event.is_string()) return {};
                const auto name = event.get<std::string>();
                if (!Id(name) || name.size() >= 64 || Contains(x.events, name)) return {};
                x.events.push_back(name);
            }
            const auto& starts = c.at("start_pos");
            if (!starts.is_object() || starts.size() > 128) return {};
            for (const auto& [banc, start] : starts.items()) {
                if (!Id(banc) || banc.size() >= 64 || !start.is_string()) return {};
                const auto name = start.get<std::string>();
                if (!Id(name) || name.size() >= 64) return {};
                x.start_pos.emplace(banc, name);
            }
            k.cross = std::move(x);
        }
        return k;
    } catch (...) { return {}; }
}
bool Reason(std::string_view r) {
    for (const auto v : {"loading", "event", "pause", "state", "airborne", "scene", "busy", "unready", "disabled", "unavailable", "rate", "age"})
        if (r == v) return true;
    return false;
}
Json EncodePacket(const Packet& p, bool server) {
    Json j{{"type", p.request ? "teleport_request" : "teleport_to"},
           {server ? "origin" : "target", p.peer}, {"request_id", p.id}};
    if (!p.request) {
        j["status"] = p.ok ? "ok" : "denied";
        if (p.ok) {
            j["banc"] = p.banc; j["room"] = p.room ? Json(*p.room) : Json(nullptr);
            j["pos"] = p.pos; j["yaw"] = p.yaw; j["age"] = p.child ? "child" : "adult";
        } else j["reason"] = p.reason;
    }
    return j;
}
std::optional<Packet> DecodePacket(const Json& j, bool server) {
    try {
        if (!j.is_object() || j.dump().size() > 512) return {};
        Packet p;
        const auto type = j.at("type").get<std::string>();
        if (type != "teleport_request" && type != "teleport_to") return {};
        p.request = type == "teleport_request";
        p.peer = j.at(server ? "origin" : "target").get<std::string>();
        const auto id = Number(j.at("request_id"), MaxId, false);
        if (!id || !*id || !Id(p.peer)) return {};
        p.id = *id;
        if (!p.request) {
            const auto status = j.at("status").get<std::string>();
            if (status != "ok" && status != "denied") return {};
            p.ok = status == "ok";
            if (p.ok) {
                p.banc = j.at("banc").get<std::string>();
                if (p.banc.empty() || !BancName(p.banc)) return {};
                if (!j.at("room").is_null()) {
                    const auto n = Number(j.at("room"), 255, false);
                    if (!n) return {};
                    p.room = static_cast<u8>(*n);
                }
                const auto& pos = j.at("pos");
                if (!pos.is_array() || pos.size() != 3) return {};
                for (size_t i = 0; i < 3; ++i) {
                    if (!Real(pos[i], 100000)) return {};
                    p.pos[i] = pos[i].get<float>();
                }
                if (!Real(j.at("yaw"), 360)) return {};
                p.yaw = j.at("yaw").get<float>();
                const auto age = j.at("age").get<std::string>();
                if (age != "child" && age != "adult") return {};
                p.child = age == "child";
            } else {
                p.reason = j.at("reason").get<std::string>();
                if (!Reason(p.reason)) return {};
            }
        }
        if (j.size() != (p.request ? 3 : p.ok ? 9 : 5)) return {};
        return p;
    } catch (...) { return {}; }
}
bool Within(const Vec& a, const Vec& b, const std::array<float, 2>& t) {
    return Position(a) && Position(b) && std::hypot(a[0] - b[0], a[2] - b[2]) <= t[0] && std::abs(a[1] - b[1]) <= t[1];
}
void History::Step(const Sample& s, const Keys& k) {
    const bool clear = !s.loading && !s.banc.empty() && s.resident && s.actor_pos &&
        s.event_running == 0 && s.block_bools_ok == true && s.command &&
        (Contains(k.requester_commands, *s.command) || Contains(k.answer_commands, *s.command)) &&
        Within(s.info_pos, *s.actor_pos, {k.agree_epsilon, k.agree_epsilon});
    if (!clear) { Reset(); return; }
    if (!previous || previous->resident != s.resident || previous->banc != s.banc || previous->child != s.child ||
        std::abs(anchor[1] - s.info_pos[1]) > k.settle_epsilon) {
        // Vertical only: a target walking on level ground settles; a fall or a jump does not.
        frames = 1; anchor = s.info_pos;
    } else frames = std::min(frames + 1, k.settle_frames);
    previous = s;
}
std::string Ready(const Sample& s, const History& h, const Keys& k, bool answer,
                  const std::vector<std::string>& exempt_bancs) {
    if (!k.enabled) return "disabled";
    if (s.loading) return "loading";
    if (!s.event_running || !s.block_bools_ok || !s.actor_pos || !s.resident || !s.command ||
        !Position(s.info_pos) || !std::isfinite(s.yaw) || std::abs(s.yaw) > 360 || s.banc.empty()) return "unready";
    if (*s.event_running) return "event";
    if (!*s.block_bools_ok) return "pause";
    if ((answer || !Contains(exempt_bancs, s.banc)) &&
        (!Contains(k.allow_bancs, s.banc) || Contains(k.deny_bancs, s.banc))) return "scene";
    if (!Contains(answer ? k.answer_commands : k.requester_commands, *s.command)) return "state";
    if (!Within(s.info_pos, *s.actor_pos, {k.agree_epsilon, k.agree_epsilon})) return "unready";
    if (h.frames < k.settle_frames) return "airborne";
    return {};
}
ApplyTarget MatrixCall(u64 entry, u64 sentinel, u64 sp, u64 actor, const Packet& p) {
    ApplyTarget t{true, false, 0, 0, entry, sentinel, sp, {actor, sp + 0x40, 0}};
    t.stores = {{sp, 0}, {sp + 8, 0}, {sp + 16, 0}, {sp + 24, 0}};
    const auto m = YawMatrix(p.pos, p.yaw);
    for (size_t i = 0; i < 6; ++i)
        t.stores.emplace_back(sp + 0x40 + i * 8, u64{std::bit_cast<u32>(m[i * 2])} | (u64{std::bit_cast<u32>(m[i * 2 + 1])} << 32));
    return t;
}
std::optional<std::string> ResolveTrigger(std::string_view line, const std::vector<Peer>& peers) {
    if (line.ends_with('\r')) line.remove_suffix(1);
    if (!line.starts_with("teleport ") || line.size() > 256) return {};
    const auto target = line.substr(9);
    std::optional<std::string> result;
    for (const auto& p : peers) {
        if (target == "any" || target == p.name || (target.size() >= 4 && p.id.starts_with(target))) {
            if (result && *result != p.id) return {};
            result = p.id;
        }
    }
    return result;
}
TriggerFile::TriggerFile(std::string path_) : path{std::move(path_)} {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return;
    std::ifstream in{path, std::ios::binary | std::ios::ate};
    if (!in || in.tellg() < 0) return;
    offset = static_cast<u64>(in.tellg());
    if (offset) {
        in.seekg(static_cast<std::streamoff>(offset - 1));
        overlong = in.get() != '\n'; // discard an incomplete pre-session line as well
    }
}
std::vector<std::string> TriggerFile::Poll(u64 now) {
    std::vector<std::string> lines;
    if (path.empty() || now - last_poll < 250) return lines;
    last_poll = now;
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return lines;
    std::ifstream in{path, std::ios::binary};
    if (!in) return lines;
    in.seekg(static_cast<std::streamoff>(offset));
    char c;
    for (size_t i = 0; i < 4096 && lines.size() < 16 && in.get(c); ++i) {
        ++offset;
        if (c == '\n') {
            if (!overlong) lines.push_back(partial);
            partial.clear(); overlong = false;
        } else if (partial.size() < 256) partial.push_back(c);
        else overlong = true;
    }
    return lines;
}
void Controller::Finish(const Log& log, std::string_view why) {
    log(fmt::format("OoT co-op: teleport refused id={} reason={}", destination.id, why));
    phase = Phase::Idle; resident.reset();
}
bool Controller::Trigger(std::string target, u64 now, const Log& log) {
    if (Busy()) { log("OoT co-op: teleport refused reason=busy"); return false; }
    if (!keys.enabled || !Id(target) || next_id > MaxId || output.size() >= 32) {
        log(fmt::format("OoT co-op: teleport refused reason={}", !keys.enabled ? "disabled" :
            !Id(target) ? "unavailable (invalid target)" : next_id > MaxId ? "request id exhausted" : "busy (output queue full)"));
        return false;
    }
    checkpoint.reset();
    cross_active = false;
    source = history.previous;
    destination = {}; destination.request = true; destination.id = next_id++; destination.peer = std::move(target);
    started = now; phase = Phase::Waiting;
    output.push_back(destination);
    log(fmt::format("OoT co-op: teleport request sent id={} target={}", destination.id, destination.peer.substr(0, 8)));
    return true;
}
void Controller::Receive(const Packet& p, u64 now, const Log& log) {
    if (p.request) {
        if (requests.size() >= 16 || output.size() >= 16 || (!seen.contains(p.peer) && seen.size() >= 128) || p.id <= seen[p.peer]) return;
        seen[p.peer] = p.id;
        requests.push_back({p, now});
    } else if (phase == Phase::Waiting && p.id == destination.id && p.peer == destination.peer && now - started <= keys.reply_timeout_ms) {
        destination = p; arrived_at = now; phase = Phase::Destination;
        log(fmt::format("OoT co-op: teleport answer received id={} origin={} status={} reason={} banc={} age={}", p.id, p.peer.substr(0, 8),
            p.ok ? "ok" : "denied", p.reason, p.banc, p.child ? "child" : "adult"));
    }
}
void Controller::Tick(u64 now, const Log& log) {
    if (phase == Phase::Waiting && now - started > keys.reply_timeout_ms) Finish(log, "reply timeout");
    if (CrossActive() && now - cross_started > keys.cross->load_timeout_ms)
        CrossFail(log, "load timeout");
}
bool Controller::Frame(const Sample& s, u64 now, bool can_call, Port& port) {
    ++frame;
    history.Step(s, keys);
    for (const auto& req : requests) {
        if (output.size() >= 32) break;
        Packet answer; answer.id = req.packet.id; answer.peer = req.packet.peer;
        answer.reason = now - req.at > 2000 ? "unready" : Ready(s, history, keys, true);
        answer.ok = answer.reason.empty(); answer.banc = s.banc; answer.child = s.child; answer.pos = s.info_pos; answer.yaw = s.yaw;
        if (answer.ok)
            port.log(fmt::format("OoT co-op: teleport answer sent id={} status=ok banc={} pos=({},{},{}) yaw={} age={}",
                answer.id, s.banc, s.info_pos[0], s.info_pos[1], s.info_pos[2], s.yaw, s.child ? "child" : "adult"));
        else port.log(fmt::format("OoT co-op: teleport answer sent id={} status=denied reason={} {}", answer.id, answer.reason, Details(s)));
        output.push_back(std::move(answer));
    }
    requests.clear();
    if (checkpoint && frame - checkpoint->frame >= (checkpoint->next ? 180 : 60)) {
        const auto& c = *checkpoint;
        const bool valid = s.resident == c.actor && port.validate(c.actor) && s.banc == c.dest.banc && s.child == c.dest.child && !s.loading;
        const bool stable = valid && s.actor_pos && Within(s.info_pos, c.dest.pos, keys.tolerance) && Within(*s.actor_pos, c.dest.pos, keys.tolerance);
        port.log(fmt::format("OoT co-op: teleport settled id={} +{}s {} {}", c.dest.id, c.next ? 3 : 1, stable ? "stable" : "moved", Details(s)));
        if (checkpoint->next++) checkpoint.reset();
    }
    if (phase == Phase::Reserve || phase == Phase::Load) return CrossFrame(s, now, can_call, port);
    if ((phase == Phase::Waiting || phase == Phase::Destination) && source &&
        (s.loading || source->banc != s.banc || source->child != s.child || source->resident != s.resident)) {
        Finish(port.log, "source scene/age/actor changed since request " + Details(s));
        return false;
    }
    if (phase == Phase::Destination) {
        const auto refuse = [&](std::string why) { Finish(port.log, why + " " + Details(s)); };
        if (now - arrived_at > keys.apply_timeout_ms) { refuse("stale answer"); return false; }
        if (!destination.ok) { refuse(destination.reason); return false; }
        const bool cross = destination.banc != s.banc;
        if (cross && (!keys.cross || !keys.cross->enabled || !Contains(keys.allow_bancs, destination.banc) ||
            Contains(keys.deny_bancs, destination.banc) || destination.banc.size() >= 64 ||
            (!keys.cross->explicit_position && !keys.cross->start_pos.contains(destination.banc)))) {
            refuse("scene (cross-scene disabled or unreviewed destination)"); return false;
        }
        if (destination.child != s.child) { refuse("age"); return false; }
        if (destination.room) { refuse("scene (rooms unsupported)"); return false; }
        const auto why = Ready(s, history, keys, false, exempt_bancs);
        if (!why.empty()) { refuse(why); return false; }
        if (!can_call) return false;
        if (!s.resident || !port.validate(*s.resident)) { refuse("actor changed"); return false; }
        resident = s.resident; scene = s.banc; child = s.child;
        if (cross) {
            cross_active = true; cross_started = now; cross_step = CrossStep::Banc; event_index = 0;
            event_live = reasserted = loading_on = switched = loading_off = old_gone = false;
            arrival_frames = 0; new_resident.reset(); abandoned = false; phase = Phase::Reserve;
            cross_from = s.info_pos;
            return CrossIssue(port);
        }
        if (!port.call(*resident, destination)) { refuse("call unavailable or actor changed"); return false; }
        phase = Phase::Calling; abandoned = false; issued_frame = frame;
        port.log(fmt::format("OoT co-op: teleport warp issued id={} actor={:#x} generation={:#x} from=({},{},{}) to=({},{},{}) yaw={}",
            destination.id, resident->actor, resident->generation, s.info_pos[0], s.info_pos[1], s.info_pos[2],
            destination.pos[0], destination.pos[1], destination.pos[2], destination.yaw));
        return true;
    }
    if (phase == Phase::Verify) {
        std::string failure;
        if (!resident || s.resident != resident || !port.validate(*resident)) failure = "actor changed";
        else if (s.banc != scene || s.child != child || s.loading) failure = "scene/age/loading changed";
        const bool arrived = failure.empty() && s.actor_pos && Within(*s.actor_pos, destination.pos, keys.tolerance) && Within(s.info_pos, destination.pos, keys.tolerance);
        if (failure.empty() && !arrived && frame - verify_start >= keys.verify_frames) failure = "verification timeout";
        if (arrived || !failure.empty()) {
            port.log(fmt::format("OoT co-op: teleport {}{} id={} frames={} error=({},{},{}) reason={} {}", cross_active ? "cross " : "", arrived ? "arrived" : "failed", destination.id,
                frame - issued_frame, s.info_pos[0] - destination.pos[0], s.info_pos[1] - destination.pos[1], s.info_pos[2] - destination.pos[2], failure, Details(s)));
            if (arrived) checkpoint = Checkpoint{destination, *resident, frame, 0};
            phase = Phase::Idle; resident.reset(); history.Reset();
            if (arrived && port.arrived) port.arrived();
        }
    }
    return false;
}
void Controller::Returned(u64 result, const Log& log) {
    if (phase == Phase::CrossCalling) {
        static constexpr std::array names{"reserve banc", "reserve start", "reserve position", "reserve fade",
            "reserve control", "event ctor", "event request", "event live re-assertion"};
        log(fmt::format("OoT co-op: teleport cross {} id={} w0={:#x} name={}", names[static_cast<size_t>(cross_step)],
            destination.id, static_cast<u32>(result),
            cross_step == CrossStep::Constructor || cross_step == CrossStep::Event ? keys.cross->events[event_index] : ""));
        if (abandoned) { phase = Phase::Idle; resident.reset(); return; }
        phase = Phase::Reserve;
        switch (cross_step) {
        case CrossStep::Banc: cross_step = CrossStep::Start; break;
        case CrossStep::Start: cross_step = keys.cross->explicit_position ? CrossStep::Position : CrossStep::Fade; break;
        case CrossStep::Position: cross_step = CrossStep::Fade; break;
        case CrossStep::Fade: cross_step = CrossStep::Control; break;
        case CrossStep::Control: cross_step = CrossStep::Constructor; break;
        case CrossStep::Constructor: cross_step = CrossStep::Event; break;
        case CrossStep::Event:
            if (result & 1) {
                log(fmt::format("OoT co-op: teleport cross event accepted id={} name={}", destination.id, keys.cross->events[event_index]));
                phase = Phase::Load; history.Reset();
            } else if (++event_index < keys.cross->events.size()) cross_step = CrossStep::Constructor;
            else CrossFail(log, "event refused");
            break;
        case CrossStep::Reassert: phase = Phase::Load; break;
        }
        return;
    }
    if (phase != Phase::Calling) return;
    log(fmt::format("OoT co-op: teleport returned id={} w0={:#x}", destination.id, static_cast<u32>(result)));
    if (abandoned) { phase = Phase::Idle; resident.reset(); }
    else { phase = Phase::Verify; verify_start = frame; }
}
void Controller::Invalidate(const Log& log, std::string_view why, bool disconnected) {
    if (Busy() && !abandoned) log(fmt::format("OoT co-op: teleport {}failed id={} reason={}", cross_active ? "cross " : "", destination.id, why));
    if (Calling()) abandoned = true;
    else { phase = Phase::Idle; resident.reset(); }
    requests.clear(); output.clear(); history.Reset(); checkpoint.reset();
    if (disconnected) seen.clear();
}
void Controller::CrossFail(const Log& log, std::string_view why) {
    if (!abandoned) log(fmt::format("OoT co-op: teleport cross failed id={} reason={}", destination.id, why));
    if (Calling()) abandoned = true;
    else { phase = Phase::Idle; resident.reset(); }
    history.Reset(); arrival_frames = 0;
}
bool Controller::CrossIssue(Port& port) {
    if (!port.cross_call || !port.cross_call(cross_step, event_index, destination)) {
        CrossFail(port.log, "call unavailable / entry or scratch invalid"); return false;
    }
    phase = Phase::CrossCalling;
    return true;
}
bool Controller::Rebinding(const Sample& s, u64 now, bool same_manager, const Log& log) {
    if (!TransitionWait()) return false;
    if (!same_manager || !event_live || !reasserted || s.banc.empty() ||
        (s.banc != scene && s.banc != destination.banc) || s.child != child ||
        (!s.loading && !loading_on) || now - cross_started > keys.cross->load_timeout_ms) {
        CrossFail(log, "binding lost: save/title identity unproven"); return false;
    }
    // Keep only value snapshots; every subsequent call resolves the current manager afresh.
    history.Reset(); arrival_frames = 0;
    return true;
}
bool Controller::CrossFrame(const Sample& s, u64 now, bool can_call, Port& port) {
    if (now - cross_started > keys.cross->load_timeout_ms) { CrossFail(port.log, "load timeout"); return false; }
    if (s.child != child) { CrossFail(port.log, "age changed"); return false; }
    if (s.banc != scene && s.banc != destination.banc) { CrossFail(port.log, "unexpected banc"); return false; }
    if (phase == Phase::Reserve) {
        if (s.banc != scene || s.loading || s.resident != resident || !resident || !port.validate(*resident) ||
            !Ready(s, history, keys, false, exempt_bancs).empty()) {
            CrossFail(port.log, "source changed during reservation"); return false;
        }
        return can_call && CrossIssue(port);
    }
    const auto receipt = [&](std::string_view message) {
        port.log(fmt::format("OoT co-op: teleport cross {} id={} ms={}", message, destination.id, now - started));
    };
    if (!event_live && s.event_running && *s.event_running) { event_live = true; receipt("event live"); }
    if (!loading_on && s.loading) { loading_on = true; receipt("loading on"); }
    if (!switched && s.banc == destination.banc) { switched = true; receipt("banc switched"); }
    if (switched && s.banc != destination.banc) { CrossFail(port.log, "banc reverted"); return false; }
    if (!old_gone && resident && !port.validate(*resident)) { old_gone = true; receipt("old resident gone"); }
    if (loading_on && !s.loading && !loading_off) { loading_off = true; receipt("loading off"); }
    if (loading_off && s.loading) { CrossFail(port.log, "loading restarted"); return false; }
    if (event_live && !reasserted) {
        if (!can_call) return false;
        cross_step = CrossStep::Reassert;
        reasserted = true;
        receipt("re-assertion issued");
        return CrossIssue(port);
    }
    // Field (C16 s2): the resident player actor survives a banc change (same actor and generation), so a
    // new resident is not required. The stale-PlayingInfo guard is the actor/PlayingInfo agreement in Ready()
    // plus a position that has left the source point.
    const bool moved = !Within(s.info_pos, cross_from, keys.tolerance);
    if (!switched || !loading_on || !loading_off || !reasserted || !s.resident || !moved ||
        !Ready(s, history, keys, false, exempt_bancs).empty() || !port.validate(*s.resident)) {
        arrival_frames = 0; return false;
    }
    if (new_resident && new_resident != s.resident) {
        CrossFail(port.log, "arrival resident changed"); return false;
    }
    if (!new_resident) {
        new_resident = s.resident; arrival_frames = 0;
        port.log(fmt::format("OoT co-op: teleport cross arrival resident id={} actor={:#x} generation={:#x} ({})",
            destination.id, s.resident->actor, s.resident->generation,
            resident && *resident == *s.resident ? "kept across the load" : "new"));
    }
    if (arrival_frames < keys.cross->arrive_frames) ++arrival_frames;
    if (arrival_frames < keys.cross->arrive_frames || !can_call) return false;
    receipt(fmt::format("ready after {} frames", arrival_frames));
    resident = s.resident; scene = destination.banc;
    if (keys.cross->explicit_position && Within(s.info_pos, destination.pos, keys.tolerance) &&
        Within(*s.actor_pos, destination.pos, keys.tolerance)) {
        receipt("arrived (no correction)");
        checkpoint = Checkpoint{destination, *resident, frame, 0};
        phase = Phase::Idle; resident.reset(); history.Reset();
        if (port.arrived) port.arrived();
        return false;
    }
    if (!port.call(*resident, destination)) { CrossFail(port.log, "correction unavailable or actor changed"); return false; }
    phase = Phase::Calling; issued_frame = frame;
    receipt("correction issued");
    return true;
}
void Controller::PeerGone(std::string_view peer, const Log& log) {
    seen.erase(std::string{peer});
    std::erase_if(requests, [&](const Incoming& r) { return r.packet.peer == peer; });
    if (Busy() && destination.peer == peer) Invalidate(log, "peer unavailable");
}
} // namespace Core::OotCoop::Teleport
