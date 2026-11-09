// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <numbers>
#include "common/logging.h"
#include "core/arm/dynarmic/oot_coop_room.h"
#include "core/arm/dynarmic/oot_coop_sync.h"
#include "core/arm/dynarmic/oot_coop_ux.h"

namespace Core::OotCoop::Room {
u64 NowMs() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
void Dispatch(std::vector<Notice> notices) {
    for (auto& n : notices) {
        if (!n.text.empty()) Ux::Notify(std::move(n.id), std::move(n.text));
        if (!n.log.empty()) LOG_INFO(Core_ARM, "{}", n.log);
    }
}
void PresenceBook::Log(Peer& p, std::string_view action, std::string_view disposition,
                       std::vector<Notice>& out) {
    if (log_lines++ < 512)
        out.push_back({{}, {}, fmt::format("OoT co-op: presence {} {} {} ({})",
            p.view.origin.substr(0, 8), p.view.name, action, disposition)});
}
void PresenceBook::Transition(Peer& p, u64 now, std::vector<Notice>& out) {
    const std::string_view action = !p.view.online ? "disconnected" :
                                   p.ever_online ? "reconnected" : "connected";
    if (p.toasted && p.toasted_online == p.view.online) {
        Log(p, action, "suppressed", out);
        return;
    }
    if (p.toasted && now - p.last_toast < 10000) {
        Log(p, action, "held", out);
        return;
    }
    if (!p.toasted && !p.view.online) return;
    p.toasted = true;
    p.toasted_online = p.view.online;
    p.ever_online |= p.view.online;
    p.last_toast = now;
    out.push_back({fmt::format("presence/{}/{}", p.view.origin, ++p.serial),
                   fmt::format("{} {}", p.view.name, action), {}});
    Log(p, action, "toast", out);
}
std::vector<Notice> PresenceBook::Receive(const nlohmann::json& packet, std::string_view name, u64 now) {
    std::vector<Notice> out;
    if (!packet.is_object() || packet.value("type", std::string{}) != "client_state" ||
        !packet.contains("origin") || !packet["origin"].is_string() ||
        !packet.contains("state") || !packet["state"].is_object()) return out;
    const auto origin = packet["origin"].get<std::string>();
    if (origin.empty() || origin.size() > 128) return out;
    const auto& state = packet["state"];
    std::scoped_lock lock{mutex};
    if (!peers.contains(origin) && peers.size() >= 128) return out;
    auto& p = peers[origin];
    const bool online = !state.contains("online") || state["online"] != false;
    const bool changed = p.view.origin.empty() || p.view.online != online;
    p.view.origin = origin;
    p.view.name = Ux::CleanName(name);
    p.view.seen = now;
    p.view.online = online;
    // Re-announcement after our outage is a silent new baseline, including a held reconnect.
    if (p.awaiting && online) p.toasted_online = true;
    p.awaiting = false;
    if (changed) Transition(p, now, out);
    if (state.contains("finished") && state["finished"].is_boolean() &&
        state["finished"] == true && !p.finished) {
        p.finished = true;
        out.push_back({"finish/" + origin, p.view.name + " finished the game", {}});
    }
    return out;
}
std::vector<Notice> PresenceBook::Tick(u64 now) {
    std::vector<Notice> out;
    std::scoped_lock lock{mutex};
    for (auto& [id, p] : peers) {
        if (reconcile && now >= *reconcile && p.awaiting) {
            p.awaiting = false;
            p.view.online = false;
            Transition(p, now, out);
        } else if (!lost && !p.awaiting && p.toasted && p.view.online != p.toasted_online &&
                   now - p.last_toast >= 10000) Transition(p, now, out);
    }
    if (reconcile && now >= *reconcile) reconcile.reset();
    return out;
}
std::vector<Notice> PresenceBook::Lost(u64 now) {
    std::scoped_lock lock{mutex};
    if (!ready) return {};
    ready = false;
    lost = true;
    reconcile.reset();
    for (auto& [id, p] : peers) p.awaiting = p.view.online;
    return {{fmt::format("relay/lost/{}", ++relay_serial), "Relay connection lost", {}}};
}
std::vector<Notice> PresenceBook::Ready(u64 now) {
    std::scoped_lock lock{mutex};
    if (ready) return {};
    ready = true;
    lost = false;
    const bool again = std::exchange(ever_ready, true);
    if (!again) return {};
    reconcile = now + 5000;
    return {{fmt::format("relay/ready/{}", ++relay_serial), "Reconnected to relay", {}}};
}
void PresenceBook::Pose(std::string_view origin, std::string_view banc, bool child, bool loading, u64 now) {
    std::scoped_lock lock{mutex};
    const auto it = peers.find(origin);
    if (it == peers.end()) return;
    auto& v = it->second.view;
    v.banc = banc;
    v.child = child;
    v.loading = loading;
    v.has_pose = true;
    v.seen = std::max(v.seen, now);
}
std::vector<RoomPeer> PresenceBook::Peers() const {
    std::scoped_lock lock{mutex};
    std::vector<RoomPeer> out;
    for (const auto& [id, p] : peers) out.push_back(p.view);
    return out;
}
std::string SyncResult::Text() const {
    if (!flags && !items) return {};
    std::string text = "Synced from team: ";
    if (flags) text += fmt::format("{} flag{}", flags, flags == 1 ? "" : "s");
    if (items) text += fmt::format("{}{} item{}", flags ? ", " : "", items, items == 1 ? "" : "s");
    return text;
}
void SyncTally::Snapshot(u64 first, u64 last) {
    std::scoped_lock lock{mutex};
    if (first <= last) ranges.push_back({first, last, {++serial, 0, 0, last - first + 1}, {}});
}
void SyncTally::Drained(u64 index, std::optional<Kind> kind, bool queued, u64 now) {
    std::scoped_lock lock{mutex};
    for (auto& r : ranges) {
        if (index < r.first || index > r.last || r.drained) continue;
        if (queued && kind) {
            switch (*kind) {
            case Kind::Bool: case Kind::Bit: case Kind::Quest: case Kind::Int: case Kind::Enum:
                ++r.result.flags; break;
            case Kind::Item: case Kind::PouchAdd: case Kind::PouchTotal: ++r.result.items; break;
            default: break;
            }
        }
        if (index == r.last) r.drained = now;
        break;
    }
}
std::vector<SyncResult> SyncTally::Settle(u64 now, bool idle) {
    std::scoped_lock lock{mutex};
    std::vector<SyncResult> out;
    for (auto it = ranges.begin(); it != ranges.end();) {
        if (it->drained && (idle || now - *it->drained >= 15000)) {
            out.push_back(it->result);
            it = ranges.erase(it);
        } else ++it;
    }
    return out;
}
bool FinishWatch::Sample(std::optional<bool> value) {
    if (!value) return false;
    const bool fire = last && !*last && *value && !finished;
    last = value;
    finished |= fire;
    return fire;
}
std::optional<u32> DecodeFinish(const nlohmann::json& value) {
    if (!value.is_object() || value.size() != 1 || !value.contains("bool") ||
        !value["bool"].is_string()) return {};
    const auto s = value["bool"].get<std::string>();
    if (s.size() != 10 || !s.starts_with("0x")) return {};
    u32 key{};
    const auto [end, ec] = std::from_chars(s.data() + 2, s.data() + s.size(), key, 16);
    if (ec != std::errc{} || end != s.data() + s.size() || !key) return {};
    return key;
}
namespace {
bool Valid(const MapTransform& m) {
    for (const float f : {m.centre[0], m.centre[1], m.half[0], m.half[1], m.world_centre[0],
                          m.world_centre[1], m.scale, m.yaw_sign, m.x_sign, m.z_sign})
        if (!std::isfinite(f)) return false;
    return m.scale > 0 && m.half[0] > 0 && m.half[1] > 0 &&
        m.centre[0] - m.half[0] >= 0 && m.centre[0] + m.half[0] <= 1 &&
        m.centre[1] - m.half[1] >= 0 && m.centre[1] + m.half[1] <= 1 &&
        std::abs(m.yaw_sign) == 1 && std::abs(m.x_sign) == 1 && std::abs(m.z_sign) == 1;
}
std::string Ascii(std::string_view s, size_t max) {
    std::string out;
    for (unsigned char c : s) {
        if (out.size() == max) break;
        out += c >= 32 && c < 127 ? static_cast<char>(c) : '?';
    }
    return out;
}
std::string Area(const RoomPeer& p, const Config& c) {
    if (!p.has_pose) return "-";
    if (p.loading) return "loading";
    const auto it = c.area_names.find(p.banc);
    return it == c.area_names.end() ? PrettyArea(p.banc) : it->second;
}
}
std::optional<Point2> PlaceMarker(const MapTransform& m, const std::array<float, 3>& local,
                                float yaw_degrees, const std::array<float, 3>& peer) {
    if (!Valid(m) || !std::isfinite(yaw_degrees)) return {};
    for (float f : local) if (!std::isfinite(f)) return {};
    for (float f : peer) if (!std::isfinite(f)) return {};
    double x = peer[0] - (m.player_centre ? local[0] : m.world_centre[0]);
    double z = peer[2] - (m.player_centre ? local[2] : m.world_centre[1]);
    if (m.swap_xz) std::swap(x, z);
    x *= m.x_sign;
    z *= m.z_sign;
    const double angle = m.player_rotation ? yaw_degrees * m.yaw_sign * std::numbers::pi / 180 : 0;
    const double dx = (x * std::cos(angle) - z * std::sin(angle)) * m.scale;
    const double dy = (x * std::sin(angle) + z * std::cos(angle)) * m.scale;
    // Map units are normalized half-extents: one unit reaches the clip boundary.
    const double distance = m.circle ? std::hypot(dx, dy) : std::max(std::abs(dx), std::abs(dy));
    if (!std::isfinite(distance)) return {};
    const double divisor = std::max(1.0, distance);
    return Point2{static_cast<float>(m.centre[0] + dx / divisor * m.half[0]),
                  static_cast<float>(m.centre[1] + dy / divisor * m.half[1]), distance > 1};
}
std::optional<MapTransform> DecodeMinimap(const nlohmann::json& v) {
    static const std::vector<std::string> fields{"centre", "half", "world_centre", "scale", "clip",
        "centre_source", "rotation", "yaw_sign", "swap_xz", "x_sign", "z_sign"};
    if (!v.is_object() || v.size() != fields.size()) return {};
    for (const auto& f : fields) if (!v.contains(f)) return {};
    MapTransform m;
    const auto pair = [&](const char* key, std::array<float, 2>& out) {
        const auto& a = v[key];
        if (!a.is_array() || a.size() != 2) return false;
        for (size_t i = 0; i < 2; ++i) {
            if (!a[i].is_number()) return false;
            out[i] = a[i].get<float>();
        }
        return true;
    };
    if (!pair("centre", m.centre) || !pair("half", m.half) || !pair("world_centre", m.world_centre)) return {};
    for (const auto* f : {"scale", "yaw_sign", "x_sign", "z_sign"}) if (!v[f].is_number()) return {};
    if ((v["clip"] != "circle" && v["clip"] != "rectangle") ||
        (v["centre_source"] != "player" && v["centre_source"] != "fixed") ||
        (v["rotation"] != "north-up" && v["rotation"] != "player-yaw") || !v["swap_xz"].is_boolean()) return {};
    m.scale = v["scale"].get<float>(); m.yaw_sign = v["yaw_sign"].get<float>();
    m.x_sign = v["x_sign"].get<float>(); m.z_sign = v["z_sign"].get<float>();
    m.circle = v["clip"] == "circle"; m.player_centre = v["centre_source"] == "player";
    m.player_rotation = v["rotation"] == "player-yaw"; m.swap_xz = v["swap_xz"].get<bool>();
    return Valid(m) ? std::optional{m} : std::nullopt;
}
Config DecodeConfig(const nlohmann::json& d, std::string_view build, bool minimap_enabled) {
    Config c;
    if (!d.is_object()) return c;
    if (d.contains("finish")) c.finish = DecodeFinish(d["finish"]);
    if (d.contains("area_names") && d["area_names"].is_object())
        for (const auto& [key, value] : d["area_names"].items()) {
            if (c.area_names.size() == 4096) break;
            if (key.size() <= 128 && value.is_string()) c.area_names[key] = Ascii(value.get<std::string>(), 56);
        }
    if (minimap_enabled && d.contains("builds") && d["builds"].is_object()) {
        const auto it = d["builds"].find(std::string{build});
        if (it != d["builds"].end() && it->is_object() && it->contains("minimap"))
            c.minimap = DecodeMinimap((*it)["minimap"]);
    }
    return c;
}
std::string AreaText(const RoomPeer& peer, const Config& config) { return Area(peer, config); }
std::string PrettyArea(std::string_view banc) {
    std::string s;
    for (size_t i = 0; i < banc.size() && s.size() < 56; ++i) {
        const char c = banc[i];
        if (c == '_') s += ' ';
        else {
            if (i && c >= 'A' && c <= 'Z' &&
                ((banc[i-1] >= 'a' && banc[i-1] <= 'z') ||
                 (banc[i-1] >= 'A' && banc[i-1] <= 'Z' && i+1 < banc.size() &&
                  banc[i+1] >= 'a' && banc[i+1] <= 'z'))) s += ' ';
            s += c;
        }
    }
    return s.empty() ? "-" : Ascii(s, 56);
}
std::string Seen(u64 now, u64 then) {
    const u64 seconds = (now >= then ? now - then : 0) / 1000;
    return seconds < 3 ? "now" : seconds < 60 ? fmt::format("seen {}s", seconds) : fmt::format("seen {}m", seconds / 60);
}
std::vector<std::string> WindowText(const WindowInfo& i, std::vector<RoomPeer> peers, const Config& c, u64 now) {
    auto policy = std::string_view{i.policy};
    if (policy.starts_with("flags-")) policy.remove_prefix(6);
    std::string seed = Ascii(i.seed, 8);
    if (seed.size() < 8) seed.insert(0, 8 - seed.size(), '0');
    std::vector<std::string> out{fmt::format("Room {}  seed {}  mod {}", Ascii(i.room, 18), seed, policy.substr(0, 8)),
        fmt::format("Relay {}  rtt {}", i.relay, i.rtt ? fmt::format("{} ms", *i.rtt) : "-")};
    out.push_back(fmt::format("You ({})  {} {}", Ux::CleanName(i.name),
        i.local ? (i.local->child ? "child" : "adult") : "  -  ", i.local ? Area(*i.local, c) : "-"));
    std::sort(peers.begin(), peers.end(), [](const RoomPeer& a, const RoomPeer& b) {
        if (a.online != b.online) return a.online > b.online;
        if (a.name != b.name) return a.name < b.name;
        return a.origin < b.origin;
    });
    if (peers.size() > 8) peers.resize(8);
    for (const auto& p : peers)
        out.push_back(fmt::format("{:<12} {:<7} {} {:<16} {}", Ux::CleanName(p.name).substr(0, 12),
            p.online ? "online" : "offline", p.has_pose ? (p.child ? "child" : "adult") : "  -  ",
            Area(p, c).substr(0, 16), Seen(now, p.seen)));
    for (auto& line : out) line = Ascii(line, 56);
    return out;
}
void Window::Frame(const WindowInfo& info, const PresenceBook& book, const Config& config, u64 now) {
    if (!initialized) {
        initialized = true;
        const char* v = std::getenv("EDEN_NX2_OOT_COOP_ROOM_WINDOW");
        mode = v && (std::string_view{v} == "on" || std::string_view{v} == "off") ? v : "trigger";
        if (const char* path = std::getenv("EDEN_NX2_OOT_COOP_ROOM_TRIGGER"); path && *path) trigger = path;
        else if (const char* keys = std::getenv("EDEN_NX2_OOT_COOP_KEYS"); keys && *keys)
            trigger = (std::filesystem::path{keys}.parent_path() / "coop-room.show").string();
        LOG_INFO(Core_ARM, "OoT co-op: room window mode {}", mode);
    }
    if (!polled || now - *polled >= 1000) {
        polled = now;
        std::error_code error;
        const bool visible = mode == "on" || (mode == "trigger" && !trigger.empty() && std::filesystem::exists(trigger, error));
        if (visible != shown) {
            shown = visible;
            built.reset();
            LOG_INFO(Core_ARM, "OoT co-op: room window {}", shown ? "show" : "hide");
            if (!shown) { lines.clear(); Ux::PublishPanel({}); }
        }
    }
    if (!shown || (built && now - *built < 500)) return;
    built = now;
    auto next = WindowText(info, book.Peers(), config, now);
    if (next != lines) { lines = std::move(next); Ux::PublishPanel(lines); }
}
} // namespace Core::OotCoop::Room
