// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include "core/arm/dynarmic/oot_coop_ux.h"
#include "core/arm/dynarmic/oot_coop_room.h"

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
    std::shared_ptr<const IconPack> icons;
    std::vector<Tag> tags;
    u64 tags_ms{}, markers_ms{}, panel_generation{};
    std::vector<Marker> markers;
    std::shared_ptr<const Panel> panel;
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

std::string Toast::Text() const {
    std::string out;
    for (const auto& run : runs) out += run.text;
    if (count > 1) out += fmt::format(" x{}", count);
    return out;
}

Toast PlainToast(std::string text) {
    Toast toast;
    if (!text.empty()) toast.runs.push_back({std::move(text), Tone::Plain});
    return toast;
}

Toast ItemToast(std::string_view who, std::string_view actor, s64 count, const ItemNames& names,
                bool local) {
    const auto name = names.Find(actor);
    const s64 n = std::clamp<s64>(count, 1, 999);
    Toast toast;
    const std::string sender = local ? std::string{"You"} : CleanName(who);
    toast.runs.push_back({sender, Tone::Name});
    toast.runs.push_back({local ? " shared " : " found ", Tone::Plain});
    if (name) {
        toast.runs.push_back({*name, Tone::Item});
        toast.count = n;
        toast.icon = std::string{actor};
        toast.merge = fmt::format("{}/{}", local ? std::string{"local"} : sender, actor);
    } else {
        toast.runs.push_back({n == 1 ? std::string{"an item"} : fmt::format("{} items", n), Tone::Item});
    }
    return toast;
}

Toast RupeeToast(std::string_view who, s64 delta) {
    const s64 n = delta < 0 ? -delta : delta;
    Toast toast;
    toast.runs.push_back({CleanName(who), Tone::Name});
    toast.runs.push_back({fmt::format(" {}{} rupee{}", delta < 0 ? '-' : '+', n, n == 1 ? "" : "s"),
                          Tone::Item});
    toast.wallet = delta;
    return toast;
}

std::string ItemText(std::string_view who, std::string_view actor, s64 count,
                     const ItemNames& names, bool local) {
    return ItemToast(who, actor, count, names, local).Text();
}

std::string RupeeText(std::string_view who, s64 delta) {
    return RupeeToast(who, delta).Text();
}

bool ToastQueue::Push(std::string id, Toast toast, u64 now_ms) {
    if (toast.runs.empty() || ids.contains(id)) return false;
    const auto remember = [&] {
        ids.insert(id);
        id_order.push_back(std::move(id));
        while (id_order.size() > MaxIds) {
            ids.erase(id_order.front());
            id_order.pop_front();
        }
    };
    if (!toast.merge.empty()) {
        Expire(now_ms);
        for (auto& s : shown) {
            if (s.toast.merge != toast.merge) continue;
            s.toast.count = std::min<s64>(s.toast.count + toast.count, 999);
            s.until = std::min(std::max(s.until, now_ms + Extend), s.since + Duration + Extend);
            remember();
            return true;
        }
        for (auto& w : waiting) {
            if (w.merge != toast.merge) continue;
            w.count = std::min<s64>(w.count + toast.count, 999);
            remember();
            return true;
        }
    }
    if (waiting.size() >= MaxQueued) return false;
    remember();
    waiting.push_back(std::move(toast));
    return true;
}

void ToastQueue::Expire(u64 now_ms) {
    const bool backlog = !waiting.empty();
    std::erase_if(shown, [&](const Shown& s) {
        return s.until <= now_ms || (backlog && now_ms >= s.since + Hurry);
    });
}

std::vector<Toast> ToastQueue::Visible(u64 now_ms) {
    Expire(now_ms);
    while (shown.size() < MaxVisible && !waiting.empty()) {
        shown.push_back({std::move(waiting.front()), now_ms, now_ms + Duration});
        waiting.pop_front();
    }
    std::vector<Toast> out;
    out.reserve(shown.size());
    for (const auto& s : shown) out.push_back(s.toast);
    return out;
}

void ToastQueue::Clear() {
    shown.clear();
    waiting.clear();
    ids.clear();
    id_order.clear();
}

std::shared_ptr<const IconPack> IconPack::Decode(const Json& index, const Reader& read, std::string* why) {
    const auto fail = [&](std::string text) -> std::shared_ptr<const IconPack> {
        if (why) *why = std::move(text);
        return nullptr;
    };
    constexpr size_t MaxIcons = 4096, MaxAtlasBytes = size_t{32} << 20;
    try {
        if (!index.is_object() || index.value("format", std::string{}) != "oot-coop-icons" ||
            !index.contains("version") || !index.at("version").is_number_integer() ||
            index.at("version").get<s64>() != 1)
            return fail("not an oot-coop-icons v1 index");
        const auto& icons = index.at("icons");
        if (!icons.is_array() || icons.empty() || icons.size() > MaxIcons) return fail("bad icons list");
        std::map<std::string, u32, std::less<>> ids;
        for (const auto& icon : icons) {
            if (!icon.is_string() || !ActorName(icon.get<std::string>())) return fail("bad icon id");
            if (!ids.emplace(icon.get<std::string>(), static_cast<u32>(ids.size())).second)
                return fail("duplicate icon id");
        }
        auto pack = std::make_shared<IconPack>();
        pack->count = ids.size();
        const auto& sizes = index.at("sizes");
        if (!sizes.is_array() || sizes.empty() || sizes.size() > 8) return fail("bad sizes list");
        for (const auto& size : sizes) {
            if (!size.is_object()) return fail("bad size entry");
            const auto cell = Number(size.at("cell"), 256), columns = Number(size.at("columns"), 256),
                       rows = Number(size.at("rows"), 256);
            const auto file = size.at("file");
            if (!cell || *cell < 8 || !columns || !*columns || !rows || !*rows || !file.is_string())
                return fail("bad size fields");
            const auto name = file.get<std::string>();
            if (name.empty() || name.size() > 64 || name.front() == '.' ||
                !std::all_of(name.begin(), name.end(), [](char c) {
                    return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.';
                }))
                return fail("bad atlas file name");
            if (*columns * *rows < pack->count) return fail("atlas smaller than the icon list");
            const size_t bytes = size_t{*cell} * *columns * *cell * *rows * 4;
            if (bytes > MaxAtlasBytes) return fail("atlas too large");
            auto data = read(name, bytes);
            if (!data || data->size() != bytes) return fail(fmt::format("atlas {} missing or wrong size", name));
            pack->atlases.push_back({static_cast<u32>(*cell), static_cast<u32>(*columns),
                                     static_cast<u32>(*rows), std::move(*data)});
        }
        std::sort(pack->atlases.begin(), pack->atlases.end(),
                  [](const Atlas& a, const Atlas& b) { return a.cell < b.cell; });
        for (size_t i = 1; i < pack->atlases.size(); ++i)
            if (pack->atlases[i].cell == pack->atlases[i - 1].cell) return fail("duplicate cell size");
        const auto& actors = index.at("actors");
        if (!actors.is_object() || actors.size() > 8192) return fail("bad actors map");
        for (const auto& [actor, icon] : actors.items()) {
            if (!ActorName(actor) || !icon.is_string()) return fail("bad actor entry");
            const auto it = ids.find(icon.get<std::string>());
            if (it == ids.end()) return fail(fmt::format("actor {} names an unknown icon", actor));
            pack->actors.emplace(actor, it->second);
        }
        if (index.contains("wallet")) {
            const auto& wallet = index.at("wallet");
            if (!wallet.is_array() || wallet.size() > 16) return fail("bad wallet list");
            for (const auto& entry : wallet) {
                if (!entry.is_object() || !entry.contains("min") || !entry.contains("icon"))
                    return fail("bad wallet entry");
                const auto min = Number(entry.at("min"), 1000000);
                const auto it = entry.at("icon").is_string() ? ids.find(entry.at("icon").get<std::string>())
                                                             : ids.end();
                if (!min || !*min || it == ids.end()) return fail("bad wallet entry");
                pack->wallet.emplace_back(static_cast<s64>(*min), it->second);
            }
            std::sort(pack->wallet.begin(), pack->wallet.end());
        }
        if (why) *why = fmt::format("{} icons, {} actors, cells {}", pack->count, pack->actors.size(),
                                    fmt::join([&] {
                                        std::vector<u32> cells;
                                        for (const auto& a : pack->atlases) cells.push_back(a.cell);
                                        return cells;
                                    }(), "/"));
        return pack;
    } catch (const Json::exception& e) {
        return fail(fmt::format("index: {}", e.what()));
    }
}

std::shared_ptr<const IconPack> IconPack::Load(const std::string& dir, std::string* why) {
    const std::filesystem::path root{dir};
    const auto slurp = [](const std::filesystem::path& path, size_t limit) -> std::optional<std::vector<u8>> {
        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        if (ec || size > limit) return std::nullopt;
        std::ifstream in{path, std::ios::binary};
        if (!in) return std::nullopt;
        std::vector<u8> bytes(static_cast<size_t>(size));
        in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!in) return std::nullopt;
        return bytes;
    };
    const auto text = slurp(root / "index.json", size_t{1} << 20);
    if (!text) {
        if (why) *why = fmt::format("no icon pack at {}", dir);
        return nullptr;
    }
    const auto index = Json::parse(text->begin(), text->end(), nullptr, false);
    if (index.is_discarded()) {
        if (why) *why = fmt::format("icon pack {}: index.json is not JSON", dir);
        return nullptr;
    }
    std::string note;
    auto pack = Decode(index, [&](std::string_view name, size_t limit) {
        return slurp(root / std::string{name}, limit);
    }, &note);
    if (why) *why = fmt::format("icon pack {}: {}", dir, note);
    return pack;
}

std::optional<u32> IconPack::Find(std::string_view actor) const {
    const auto it = actors.find(actor);
    return it == actors.end() ? std::nullopt : std::optional<u32>{it->second};
}

std::optional<u32> IconPack::Wallet(s64 delta) const {
    const s64 n = delta < 0 ? -delta : delta;
    std::optional<u32> out;
    for (const auto& [min, icon] : wallet)
        if (n >= min) out = icon;
    return out;
}

const IconPack::Atlas* IconPack::Best(u32 pixels) const {
    if (atlases.empty()) return nullptr;
    const Atlas* best = &atlases.front();
    for (const auto& atlas : atlases)
        if (atlas.cell <= pixels) best = &atlas;
    return best;
}

std::vector<u8> IconPack::Icon(const Atlas& atlas, u32 index) const {
    std::vector<u8> out(size_t{atlas.cell} * atlas.cell * 4);
    if (index >= count || index >= atlas.columns * atlas.rows) return out;
    const size_t stride = size_t{atlas.columns} * atlas.cell * 4;
    const size_t x0 = size_t{index % atlas.columns} * atlas.cell * 4;
    const size_t y0 = size_t{index / atlas.columns} * atlas.cell;
    for (u32 y = 0; y < atlas.cell; ++y)
        std::copy_n(atlas.rgba.begin() + static_cast<std::ptrdiff_t>((y0 + y) * stride + x0),
                    size_t{atlas.cell} * 4, out.begin() + static_cast<std::ptrdiff_t>(size_t{y} * atlas.cell * 4));
    return out;
}

namespace {
std::filesystem::path UxSidecarPath() {
    if (const char* ux = std::getenv("EDEN_NX2_OOT_COOP_UX"); ux && *ux) return ux;
    if (const char* keys = std::getenv("EDEN_NX2_OOT_COOP_KEYS"); keys && *keys)
        return std::filesystem::path{keys}.parent_path() / "coop-ux.json";
    return {};
}
} // namespace

std::string IconDirectory() {
    if (const char* icons = std::getenv("EDEN_NX2_OOT_COOP_ICONS"); icons && *icons) return icons;
    const auto ux = UxSidecarPath();
    return ux.empty() ? std::string{} : (ux.parent_path() / "coop-icons").string();
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

std::optional<Point2> Project(const Camera& camera, const std::array<float, 3>& world, bool flip_y,
                              float* depth) {
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
    if (depth) *depth = -v[2];
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
        float depth{};
        if (const auto at = Project(camera, head, keys.flip_y, &depth))
            out.push_back({CleanName(peer.name), *at, false, depth});
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
    config.room = std::make_shared<const Room::Config>(
        Room::DecodeConfig(document, build_id, Switch("MINIMAP", false)));
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

namespace {
std::shared_ptr<const IconPack> LoadIcons(std::string& note) {
    if (!Switch("TOAST_ICONS")) {
        note = "toast icons off (EDEN_NX2_OOT_COOP_TOAST_ICONS=0)";
        return nullptr;
    }
    const auto dir = IconDirectory();
    if (dir.empty()) {
        note = "toast icons off (no icon pack path)";
        return nullptr;
    }
    auto pack = IconPack::Load(dir, &note);
    if (!pack) note = "toast icons off: " + note;
    return pack;
}
} // namespace

std::shared_ptr<const Config> LoadConfig(std::string_view build_id, std::string* why) {
    std::string icons_note;
    auto icons = LoadIcons(icons_note);
    const auto with_icons = [&](Config config) {
        config.icons = icons;
        return std::make_shared<const Config>(std::move(config));
    };
    const std::filesystem::path path = UxSidecarPath();
    if (path.empty()) {
        if (why) *why = "no sidecar path (EDEN_NX2_OOT_COOP_UX and EDEN_NX2_OOT_COOP_KEYS unset); " + icons_note;
        return with_icons({});
    }
    std::ifstream in{path};
    if (!in) {
        if (why) *why = fmt::format("no sidecar at {}; {}", path.string(), icons_note);
        return with_icons({});
    }
    std::string text;
    char c;
    while (in.get(c) && text.size() <= (4U << 20)) text.push_back(c);
    const auto document = Json::parse(text, nullptr, false);
    if (document.is_discarded()) {
        if (why) *why = fmt::format("sidecar {} is not JSON; {}", path.string(), icons_note);
        return with_icons({});
    }
    std::string note;
    auto config = with_icons(DecodeConfig(document, build_id, &note));
    if (why) *why = fmt::format("sidecar {}: {}; {}", path.string(), note, icons_note);
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
           ItemToast(Name(origin), actor, count, config ? config->names : empty));
}

void SessionUx::RupeeReceived(std::string_view origin, u64 seq, s64 delta) {
    Notify(fmt::format("{}/{}", origin, seq), RupeeToast(Name(origin), delta));
}

void SessionUx::ItemShared(std::string_view actor, s64 count) {
    static const bool enabled = Switch("TOAST_LOCAL");
    static const ItemNames empty;
    if (!enabled) return;
    Notify(fmt::format("local/{}", ++shared), ItemToast({}, actor, count, config ? config->names : empty, true));
}

bool Notify(std::string id, std::string text) {
    return Notify(std::move(id), PlainToast(std::move(text)));
}

bool Notify(std::string id, Toast toast) {
    static const bool enabled = Switch("TOAST");
    if (!enabled) return false;
    auto& g = State();
    std::scoped_lock lock{g.mutex};
    return g.toasts.Push(std::move(id), std::move(toast), NowMs());
}

void PublishIcons(std::shared_ptr<const IconPack> icons) {
    auto& g = State();
    std::scoped_lock lock{g.mutex};
    g.icons = std::move(icons);
}

void PublishTags(std::vector<Tag> tags) {
    auto& g = State();
    std::scoped_lock lock{g.mutex};
    g.tags = std::move(tags);
    g.tags_ms = NowMs();
}

void PublishMarkers(std::vector<Marker> markers) {
    auto& g = State();
    std::scoped_lock lock{g.mutex};
    g.markers = std::move(markers);
    g.markers_ms = NowMs();
}

void PublishPanel(std::vector<std::string> lines) {
    auto& g = State();
    std::scoped_lock lock{g.mutex};
    if (lines.empty()) { g.panel.reset(); return; }
    if (g.panel && g.panel->lines == lines) return;
    g.panel = std::make_shared<const Panel>(Panel{std::move(lines), ++g.panel_generation});
}

void Reset() {
    auto& g = State();
    std::scoped_lock lock{g.mutex};
    g.toasts.Clear();
    g.icons.reset();
    g.tags.clear();
    g.tags_ms = 0;
    g.markers.clear();
    g.markers_ms = 0;
    g.panel.reset();
}

OverlayFrame Current() {
    auto& g = State();
    const u64 now = NowMs();
    std::scoped_lock lock{g.mutex};
    OverlayFrame frame;
    if (!g.toasts.Empty()) {
        frame.toasts = g.toasts.Visible(now);
        frame.more = g.toasts.Waiting();
        if (!frame.toasts.empty()) frame.icons = g.icons;
    }
    if (!g.tags.empty() && now - g.tags_ms <= TagLifetimeMs) frame.tags = g.tags;
    frame.panel = g.panel;
    if (!g.markers.empty() && now - g.markers_ms <= TagLifetimeMs) frame.markers = g.markers;
    return frame;
}

} // namespace Core::OotCoop::Ux
