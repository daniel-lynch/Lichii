// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>

#include <fmt/format.h>

#include "core/arm/dynarmic/oot_coop_ux.h"

namespace Core::OotCoop::Ux {
namespace {
using Json = nlohmann::json;

u64 NowMs() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

std::optional<u64> Number(const Json& value, u64 maximum) {
    if (value.is_number_unsigned()) {
        const u64 n = value.get<u64>();
        return n <= maximum ? std::optional<u64>{n} : std::nullopt;
    }
    if (value.is_number_integer()) {
        const s64 n = value.get<s64>();
        return n >= 0 && static_cast<u64>(n) <= maximum ? std::optional<u64>{static_cast<u64>(n)}
                                                         : std::nullopt;
    }
    if (!value.is_string()) return std::nullopt;
    const auto text = value.get<std::string>();
    if (text.size() < 3 || text.size() > 18 || text[0] != '0' || (text[1] != 'x' && text[1] != 'X'))
        return std::nullopt;
    u64 n = 0;
    for (size_t i = 2; i < text.size(); ++i) {
        const char c = text[i];
        const int digit = c >= '0' && c <= '9'   ? c - '0'
                          : c >= 'a' && c <= 'f' ? c - 'a' + 10
                          : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                 : -1;
        if (digit < 0) return std::nullopt;
        n = (n << 4) | static_cast<u64>(digit);
    }
    return n <= maximum ? std::optional<u64>{n} : std::nullopt;
}

bool ActorName(std::string_view name) {
    return !name.empty() && name.size() <= 63 &&
           std::all_of(name.begin(), name.end(), [](char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '_';
           });
}

std::string Printable(std::string_view text, size_t limit) {
    std::string out;
    for (size_t i = 0; i < text.size() && out.size() < limit; ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c >= 0x20 && c < 0x7F) {
            out.push_back(static_cast<char>(c));
        } else if (c >= 0xC0 || c < 0x80) {
            out.push_back('?'); // a control byte or the lead byte of one UTF-8 sequence
        }
    }
    return out;
}

struct Global {
    std::mutex mutex;
    ToastQueue toasts;
    std::vector<Tag> tags;
    u64 tags_ms{};
};
Global& State() {
    static Global global;
    return global;
}
} // namespace

bool Switch(std::string_view name, bool fallback) {
    const auto key = fmt::format("EDEN_NX2_OOT_COOP_{}", name);
    const char* value = std::getenv(key.c_str());
    if (!value || !*value) return fallback;
    const std::string_view v{value};
    if (v == "0" || v == "off" || v == "false" || v == "no") return false;
    if (v == "1" || v == "on" || v == "true" || v == "yes") return true;
    return fallback;
}

std::string CleanName(std::string_view name) {
    auto out = Printable(name, 24);
    while (!out.empty() && out.back() == ' ') out.pop_back();
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    return out.empty() ? std::string{"Player"} : out;
}

size_t ItemNames::Load(const Json& document) {
    names.clear();
    if (!document.is_object() || !document.contains("item_names")) return 0;
    const auto& table = document.at("item_names");
    if (!table.is_object()) return 0;
    for (const auto& [actor, value] : table.items()) {
        if (!ActorName(actor) || !value.is_string()) continue;
        const auto text = Printable(value.get<std::string>(), 40);
        if (text.empty() || names.size() >= 4096) continue;
        names.emplace(actor, text);
    }
    return names.size();
}

std::optional<std::string> ItemNames::Find(std::string_view actor) const {
    const auto it = names.find(actor);
    return it == names.end() ? std::nullopt : std::optional<std::string>{it->second};
}

std::string ItemText(std::string_view who, std::string_view actor, s64 count,
                     const ItemNames& names, bool local) {
    const auto name = names.Find(actor);
    const s64 n = std::clamp<s64>(count, 1, 999);
    const std::string subject = local ? std::string{"You shared"} : CleanName(who) + " got";
    if (!name) return n == 1 ? subject + " an item" : fmt::format("{} {} items", subject, n);
    return fmt::format("{} {}x {}", subject, n, *name);
}

std::string RupeeText(std::string_view who, s64 delta) {
    const s64 n = delta < 0 ? -delta : delta;
    return fmt::format("{} {} {} rupee{}", CleanName(who), delta < 0 ? "spent" : "got", n,
                       n == 1 ? "" : "s");
}

bool ToastQueue::Push(std::string id, std::string text, u64 now_ms) {
    (void)now_ms;
    if (text.empty() || ids.contains(id)) return false;
    if (waiting.size() >= MaxQueued) return false;
    ids.insert(id);
    id_order.push_back(std::move(id));
    while (id_order.size() > MaxIds) {
        ids.erase(id_order.front());
        id_order.pop_front();
    }
    waiting.push_back(std::move(text));
    return true;
}

std::vector<std::string> ToastQueue::Visible(u64 now_ms) {
    std::erase_if(shown, [now_ms](const Shown& s) { return s.until <= now_ms; });
    while (shown.size() < MaxVisible && !waiting.empty()) {
        shown.push_back({std::move(waiting.front()), now_ms + Duration});
        waiting.pop_front();
    }
    std::vector<std::string> out;
    out.reserve(shown.size());
    for (const auto& s : shown) out.push_back(s.text);
    return out;
}

void ToastQueue::Clear() {
    shown.clear();
    waiting.clear();
    ids.clear();
    id_order.clear();
}

std::optional<CameraKeys> DecodeCamera(const Json& value) {
    if (!value.is_object()) return std::nullopt;
    CameraKeys k;
    constexpr u64 Offset = 0x10000000; // module offsets and field offsets alike stay below this
    const std::array<std::pair<std::string_view, u64 CameraKeys::*>, 24> fields{{
        {"registry_cell", &CameraKeys::registry_cell},
        {"registry_table", &CameraKeys::registry_table},
        {"mask", &CameraKeys::mask},
        {"buckets", &CameraKeys::buckets},
        {"bucket_capacity", &CameraKeys::bucket_capacity},
        {"bucket_hashes", &CameraKeys::bucket_hashes},
        {"bucket_count", &CameraKeys::bucket_count},
        {"bucket_index", &CameraKeys::bucket_index},
        {"assoc_cell", &CameraKeys::assoc_cell},
        {"assoc_slot", &CameraKeys::assoc_slot},
        {"assoc_slot_stride", &CameraKeys::assoc_slot_stride},
        {"assoc_entry", &CameraKeys::assoc_entry},
        {"assoc_entry_stride", &CameraKeys::assoc_entry_stride},
        {"projection", &CameraKeys::projection},
        {"views_cell", &CameraKeys::views_cell},
        {"views_count", &CameraKeys::views_count},
        {"views_array", &CameraKeys::views_array},
        {"view_flags", &CameraKeys::view_flags},
        {"view_special", &CameraKeys::view_special},
        {"view_special_offset", &CameraKeys::view_special_offset},
        {"view_pointer", &CameraKeys::view_pointer},
        {"view_matrix", &CameraKeys::view_matrix},
        {"projection_dirty", &CameraKeys::projection_dirty},
        {"projection_matrix", &CameraKeys::projection_matrix},
    }};
    try {
        for (const auto& [name, member] : fields) {
            if (!value.contains(std::string{name})) return std::nullopt;
            const auto n = Number(value.at(std::string{name}), Offset);
            if (!n) return std::nullopt;
            k.*member = *n;
        }
        for (const auto& [name, member] :
             std::array<std::pair<std::string_view, u32 CameraKeys::*>, 3>{{
                 {"main_hash", &CameraKeys::main_hash},
                 {"special_any", &CameraKeys::special_any},
                 {"special_all", &CameraKeys::special_all}}}) {
            if (!value.contains(std::string{name})) return std::nullopt;
            const auto n = Number(value.at(std::string{name}), 0xFFFFFFFF);
            if (!n) return std::nullopt;
            k.*member = static_cast<u32>(*n);
        }
        const auto height = [&](const char* name, float& out) {
            if (!value.contains(name)) return true;
            const auto& h = value.at(name);
            if (!h.is_number()) return false;
            out = h.get<float>();
            return std::isfinite(out) && out >= 0.0f && out <= 10.0f;
        };
        if (!height("head_child", k.head_child) || !height("head_adult", k.head_adult))
            return std::nullopt;
        if (value.contains("flip_y")) {
            if (!value.at("flip_y").is_boolean()) return std::nullopt;
            k.flip_y = value.at("flip_y").get<bool>();
        }
        for (const auto& [name, _] : value.items()) {
            const bool known =
                std::any_of(fields.begin(), fields.end(), [&](const auto& f) { return f.first == name; }) ||
                name == "main_hash" || name == "special_any" || name == "special_all" ||
                name == "head_child" || name == "head_adult" || name == "flip_y" || name == "note";
            if (!known) return std::nullopt;
        }
    } catch (const Json::exception&) {
        return std::nullopt;
    }
    return k.Valid() ? std::optional<CameraKeys>{k} : std::nullopt;
}

std::optional<Point2> Project(const Camera& camera, const std::array<float, 3>& world, bool flip_y) {
    const auto& m = camera.view;
    const float q[4]{world[0], world[1], world[2], 1.0f};
    float v[4]{0, 0, 0, 1.0f};
    for (int row = 0; row < 3; ++row) {
        v[row] = m[row * 4] * q[0] + m[row * 4 + 1] * q[1] + m[row * 4 + 2] * q[2] + m[row * 4 + 3];
    }
    if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2]) || v[2] >= 0.0f)
        return std::nullopt;
    const auto& p = camera.projection;
    float c[4];
    for (int row = 0; row < 4; ++row) {
        c[row] = p[row * 4] * v[0] + p[row * 4 + 1] * v[1] + p[row * 4 + 2] * v[2] + p[row * 4 + 3] * v[3];
        if (!std::isfinite(c[row])) return std::nullopt;
    }
    constexpr float Epsilon = 1.0f / 8388608.0f; // 2^-23, the native projection's own
    if (std::abs(c[3]) <= Epsilon) return std::nullopt;
    const float nx = c[0] / c[3], ny = c[1] / c[3];
    if (!std::isfinite(nx) || !std::isfinite(ny) || nx < -1.0f || nx > 1.0f || ny < -1.0f || ny > 1.0f)
        return std::nullopt;
    Point2 out{(nx + 1.0f) * 0.5f, flip_y ? (ny + 1.0f) * 0.5f : (1.0f - ny) * 0.5f};
    return out;
}

std::string_view CameraFailName(CameraFail fail) {
    switch (fail) {
    case CameraFail::None: return "ok";
    case CameraFail::Root: return "root";
    case CameraFail::Registry: return "registry";
    case CameraFail::NoMain: return "no-main";
    case CameraFail::Slot: return "slot";
    case CameraFail::Association: return "association";
    case CameraFail::View: return "view";
    case CameraFail::Dirty: return "dirty";
    case CameraFail::Matrix: return "matrix";
    }
    return "?";
}

std::vector<Tag> BuildTags(const Camera& camera, const CameraKeys& keys, std::string_view local_banc,
                           bool local_loading, const std::vector<TagPeer>& peers, bool crosses) {
    std::vector<Tag> out;
    if (local_banc.empty() || local_loading) return out;
    for (const auto& peer : peers) {
        if (peer.loading || peer.banc != local_banc) continue;
        const float height = peer.child ? keys.head_child : keys.head_adult;
        const std::array<float, 3> head{peer.pos[0], peer.pos[1] + height, peer.pos[2]};
        if (crosses) {
            if (const auto root = Project(camera, peer.pos, keys.flip_y)) out.push_back({"", *root, true});
            if (const auto top = Project(camera, head, keys.flip_y)) out.push_back({"", *top, true});
        }
        if (const auto at = Project(camera, head, keys.flip_y)) out.push_back({CleanName(peer.name), *at, false});
    }
    return out;
}

Config DecodeConfig(const Json& document, std::string_view build_id, std::string* why) {
    Config config;
    if (!document.is_object()) {
        if (why) *why = "not a JSON object";
        return config;
    }
    config.names.Load(document);
    std::string note = fmt::format("{} item names", config.names.Size());
    try {
        const auto builds = document.find("builds");
        if (builds != document.end() && builds->is_object()) {
            const auto build = builds->find(std::string{build_id});
            if (build != builds->end() && build->is_object() && build->contains("camera")) {
                config.camera = DecodeCamera(build->at("camera"));
                note += config.camera ? ", camera keys" : ", camera keys INVALID";
            } else {
                note += ", no camera keys for this build";
            }
        } else {
            note += ", no builds";
        }
    } catch (const Json::exception&) {
        note += ", bad builds object";
    }
    if (why) *why = note;
    return config;
}

std::shared_ptr<const Config> LoadConfig(std::string_view build_id, std::string* why) {
    std::filesystem::path path;
    if (const char* ux = std::getenv("EDEN_NX2_OOT_COOP_UX"); ux && *ux) {
        path = ux;
    } else if (const char* keys = std::getenv("EDEN_NX2_OOT_COOP_KEYS"); keys && *keys) {
        path = std::filesystem::path{keys}.parent_path() / "coop-ux.json";
    }
    if (path.empty()) {
        if (why) *why = "no sidecar path (EDEN_NX2_OOT_COOP_UX and EDEN_NX2_OOT_COOP_KEYS unset)";
        return std::make_shared<const Config>();
    }
    std::ifstream in{path};
    if (!in) {
        if (why) *why = fmt::format("no sidecar at {}", path.string());
        return std::make_shared<const Config>();
    }
    std::string text;
    char c;
    while (in.get(c) && text.size() <= (4U << 20)) text.push_back(c);
    const auto document = Json::parse(text, nullptr, false);
    if (document.is_discarded()) {
        if (why) *why = fmt::format("sidecar {} is not JSON", path.string());
        return std::make_shared<const Config>();
    }
    std::string note;
    auto config = std::make_shared<const Config>(DecodeConfig(document, build_id, &note));
    if (why) *why = fmt::format("sidecar {}: {}", path.string(), note);
    return config;
}

void SessionUx::SetName(const std::string& origin, std::string_view name) {
    std::scoped_lock lock{names_mutex};
    if (names.size() >= 64 && !names.contains(origin)) return;
    names[origin] = CleanName(name);
}

std::string SessionUx::Name(std::string_view origin) const {
    std::scoped_lock lock{names_mutex};
    const auto it = names.find(origin);
    return it != names.end() ? it->second : std::string{"Player"};
}

void SessionUx::ItemReceived(std::string_view origin, u64 seq, std::string_view actor, s64 count) {
    static const ItemNames empty;
    Notify(fmt::format("{}/{}", origin, seq),
           ItemText(Name(origin), actor, count, config ? config->names : empty));
}

void SessionUx::RupeeReceived(std::string_view origin, u64 seq, s64 delta) {
    Notify(fmt::format("{}/{}", origin, seq), RupeeText(Name(origin), delta));
}

void SessionUx::ItemShared(std::string_view actor, s64 count) {
    static const bool enabled = Switch("TOAST_LOCAL");
    static const ItemNames empty;
    if (!enabled) return;
    Notify(fmt::format("local/{}", ++shared), ItemText({}, actor, count, config ? config->names : empty, true));
}

bool Notify(std::string id, std::string text) {
    static const bool enabled = Switch("TOAST");
    if (!enabled) return false;
    auto& g = State();
    std::scoped_lock lock{g.mutex};
    return g.toasts.Push(std::move(id), std::move(text), NowMs());
}

void PublishTags(std::vector<Tag> tags) {
    auto& g = State();
    std::scoped_lock lock{g.mutex};
    g.tags = std::move(tags);
    g.tags_ms = NowMs();
}

void Reset() {
    auto& g = State();
    std::scoped_lock lock{g.mutex};
    g.toasts.Clear();
    g.tags.clear();
    g.tags_ms = 0;
}

OverlayFrame Current() {
    auto& g = State();
    const u64 now = NowMs();
    std::scoped_lock lock{g.mutex};
    OverlayFrame frame;
    if (!g.toasts.Empty()) frame.toasts = g.toasts.Visible(now);
    if (!g.tags.empty() && now - g.tags_ms <= TagLifetimeMs) frame.tags = g.tags;
    return frame;
}

} // namespace Core::OotCoop::Ux
