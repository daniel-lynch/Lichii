// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Co-op UX (host side only, no guest writes): corner toasts for received items and a projected,
// through-wall name tag above each peer's ghost. The presenter draws what Current() returns.
//
// Everything game-derived (the camera read chain, the item display names) comes from a private
// sidecar next to the co-op profile, never from source: EDEN_NX2_OOT_COOP_UX, or "coop-ux.json"
// in the directory of EDEN_NX2_OOT_COOP_KEYS:
//   {"item_names": {"<actor name>": "<display name>", ...},
//    "builds": {"<main build id>": {"camera": {...CameraKeys fields...}}}}
//
// Switches (all on unless set to 0/off/false/no):
//   EDEN_NX2_OOT_COOP_TOAST          corner toasts
//   EDEN_NX2_OOT_COOP_TOAST_LOCAL    also toast what this player shares
//   EDEN_NX2_OOT_COOP_TOAST_ICONS    item icons in toasts (needs the private icon pack, below)
//   EDEN_NX2_OOT_COOP_NAMETAG        name tags
//   EDEN_NX2_OOT_COOP_OVERLAY_SHOTS  draw the overlay into emulator screenshots too
// Off unless set to 1: EDEN_NX2_OOT_COOP_NAMETAG_DEBUG (feet/head crosses, the projection probe).
//
// The optional icon pack is a private, game-derived directory (never in source):
// EDEN_NX2_OOT_COOP_ICONS, or "coop-icons" beside the UX sidecar. See IconPack below.

#include <array>
#include <cmath>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <nlohmann/json.hpp>

#include "common/common_types.h"

namespace Core::OotCoop::Room { struct Config; }
namespace Core::OotCoop::Checks { struct Panel; }

namespace Core::OotCoop::Ux {

/// EDEN_NX2_OOT_COOP_<name>: true unless set to 0, off, false or no (or, with `fallback` false,
/// false unless set to 1, on, true or yes).
[[nodiscard]] bool Switch(std::string_view name, bool fallback = true);

/// A player name as overlay text: printable ASCII kept, anything else one '?', at most 24
/// characters; empty becomes "Player". Never interpreted as markup or a format string.
[[nodiscard]] std::string CleanName(std::string_view name);

class ItemNames {
public:
    /// document["item_names"]: actor name -> display name. Bad entries are skipped; returns the
    /// number loaded.
    size_t Load(const nlohmann::json& document);
    [[nodiscard]] std::optional<std::string> Find(std::string_view actor) const;
    [[nodiscard]] size_t Size() const { return names.size(); }

private:
    std::map<std::string, std::string, std::less<>> names;
};

/// How a run of toast text is coloured by the presenter.
enum class Tone : u8 { Plain, Name, Item, Muted };
struct Run {
    std::string text;
    Tone tone{Tone::Plain};
    bool operator==(const Run&) const = default;
};

/// One toast: coloured runs, an optional icon (an actor the icon pack maps, or a wallet delta)
/// and a merge key. Toasts with the same non-empty key merge while on screen or queued: the
/// counts add up and the presenter shows " xN" after the runs.
struct Toast {
    std::vector<Run> runs;
    std::string icon;  // actor name for the icon pack; empty: none
    s64 wallet{};      // nonzero: a rupee icon for this wallet delta
    std::string merge; // merge key; empty: never merges
    s64 count{1};      // shown as " xN" when > 1
    /// The plain text (logs, tests): runs, then " xN" when count > 1.
    [[nodiscard]] std::string Text() const;
    bool operator==(const Toast&) const = default;
};
/// A plain one-run toast (presence, sync, quest, finish notices).
[[nodiscard]] Toast PlainToast(std::string text);

/// "Anthony found Kokiri Sword" (count 1) / "Anthony found Deku Nut x5"; local: "You shared
/// Kokiri Sword". An actor the table does not name gives the explicit fallback "an item" / "N
/// items" (never a name guessed from the actor id). Same sender + actor merge.
[[nodiscard]] Toast ItemToast(std::string_view who, std::string_view actor, s64 count,
                              const ItemNames& names, bool local = false);
/// "Anthony +20 rupees" (a wallet delta; negative: "Anthony -5 rupees"), with the rupee icon.
[[nodiscard]] Toast RupeeToast(std::string_view who, s64 delta);
/// Plain-text forms of the above (kept for logs and older callers).
[[nodiscard]] std::string ItemText(std::string_view who, std::string_view actor, s64 count,
                                   const ItemNames& names, bool local = false);
[[nodiscard]] std::string RupeeText(std::string_view who, s64 delta);

/// Toasts: deduplicated by id, merged by key, shown in arrival order, at most MaxVisible at
/// once, each for Duration ms from the moment it becomes visible; while others wait, a toast
/// that has been up for Hurry ms gives way. A merge into a visible toast extends it by Extend ms
/// (never past Duration + Extend from when it appeared). Clock is the caller's (ms).
class ToastQueue {
public:
    static constexpr u64 Duration = 5000;
    static constexpr u64 Hurry = 2500;
    static constexpr u64 Extend = 2000;
    static constexpr size_t MaxVisible = 3;
    static constexpr size_t MaxQueued = 16;
    static constexpr size_t MaxIds = 4096;

    /// False for a repeated id, an empty toast or a full queue (a merge never fills the queue).
    bool Push(std::string id, Toast toast, u64 now_ms);
    bool Push(std::string id, std::string text, u64 now_ms) {
        return Push(std::move(id), PlainToast(std::move(text)), now_ms);
    }
    /// The toasts on screen now, oldest first.
    [[nodiscard]] std::vector<Toast> Visible(u64 now_ms);
    /// Toasts waiting for a slot (the presenter shows "+N more").
    [[nodiscard]] size_t Waiting() const { return waiting.size(); }
    [[nodiscard]] bool Empty() const { return shown.empty() && waiting.empty(); }
    void Clear();

private:
    struct Shown {
        Toast toast;
        u64 since;
        u64 until;
    };
    void Expire(u64 now_ms);
    std::deque<Shown> shown;
    std::deque<Toast> waiting;
    std::set<std::string> ids;
    std::deque<std::string> id_order;
};

/// The private icon pack (game-derived; loaded from disk, never in source):
///   <dir>/index.json {"format": "oot-coop-icons", "version": 1,
///                     "sizes": [{"cell": N, "file": "<name>.rgba", "columns": C, "rows": R}, ...],
///                     "icons": ["<icon id>", ...], "actors": {"<actor>": "<icon id>"},
///                     "wallet": [{"min": n, "icon": "<icon id>"}, ...]}
///   <dir>/<file>     raw RGBA8, straight alpha, (C*N) x (R*N), icon i at column i % C, row i / C.
class IconPack {
public:
    struct Atlas {
        u32 cell{}, columns{}, rows{};
        std::vector<u8> rgba;
    };
    /// Strict: any malformed field rejects the pack (`why` says what). Files are read through
    /// `read(name, limit)`, which returns nothing for a missing or larger file.
    using Reader = std::function<std::optional<std::vector<u8>>(std::string_view name, size_t limit)>;
    static std::shared_ptr<const IconPack> Decode(const nlohmann::json& index, const Reader& read,
                                                  std::string* why = nullptr);
    /// Load `dir` from disk.
    static std::shared_ptr<const IconPack> Load(const std::string& dir, std::string* why = nullptr);

    [[nodiscard]] std::optional<u32> Find(std::string_view actor) const;
    [[nodiscard]] std::optional<u32> Wallet(s64 delta) const;
    /// The atlas whose cell is the largest not above `pixels` (else the smallest).
    [[nodiscard]] const Atlas* Best(u32 pixels) const;
    /// Copy icon `index` of `atlas` into a cell x cell RGBA buffer.
    [[nodiscard]] std::vector<u8> Icon(const Atlas& atlas, u32 index) const;
    [[nodiscard]] size_t Size() const { return count; }
    [[nodiscard]] size_t Actors() const { return actors.size(); }

private:
    std::vector<Atlas> atlases; // ascending cell
    std::map<std::string, u32, std::less<>> actors;
    std::vector<std::pair<s64, u32>> wallet; // ascending min
    size_t count{};
};
/// "coop-icons" beside the UX sidecar (or EDEN_NX2_OOT_COOP_ICONS); empty when neither is set.
[[nodiscard]] std::string IconDirectory();

/// The active Main camera's read chain (static research, ghost-ux REPORT 3.3). Module offsets are
/// relative to the main base; the rest are field offsets.
struct CameraKeys {
    u64 registry_cell{};   // S = *(main + registry_cell)
    u64 registry_table{};  // T = *(S + registry_table)
    u64 mask{};            // u32 bucket mask at T + mask
    u64 buckets{};         // bucket pointers at T + buckets + 8 i
    u64 bucket_capacity{}; // u32 at B + bucket_capacity
    u64 bucket_hashes{};   // pointer to sorted u32 hashes at B + bucket_hashes
    u64 bucket_count{};    // u32 at B + bucket_count
    u64 bucket_index{};    // u32 render index at B + bucket_index
    u32 main_hash{};       // the registry name hash of the Main view
    u64 assoc_cell{};      // R = *(main + assoc_cell)
    u64 assoc_slot{};      // s32 slot at R + assoc_slot + assoc_slot_stride * index
    u64 assoc_slot_stride{};
    u64 assoc_entry{};     // A = *(R + assoc_entry + assoc_entry_stride * slot)
    u64 assoc_entry_stride{};
    u64 projection{};      // P = *(A + projection)
    u64 views_cell{};      // D = *(main + views_cell)
    u64 views_count{};     // u32 at D + views_count
    u64 views_array{};     // L = *(*(D + views_array) + 8 index)
    u64 view_flags{};      // u16 at L + view_flags
    u64 view_special{};    // pointer at L + view_special
    u64 view_special_offset{}; // V = special + view_special_offset
    u64 view_pointer{};    // otherwise V = *(L + view_pointer)
    u32 special_any{};     // the special view is used when (flags & any) && (flags & all) == all
    u32 special_all{};
    u64 view_matrix{};     // 12 floats at V + view_matrix (row-major 3x4)
    u64 projection_dirty{};  // u8 at P + projection_dirty: 1 = CPU matrix stale
    u64 projection_matrix{}; // 16 floats at P + projection_matrix (row-major 4x4)
    float head_child{1.0f};  // name-tag height above the ghost root, world units
    float head_adult{1.5f};
    bool flip_y{};           // NDC +y is down (otherwise up)
    [[nodiscard]] bool Valid() const {
        return registry_cell && registry_table && mask && main_hash && assoc_cell &&
               assoc_slot_stride && assoc_entry_stride && views_cell && views_array && view_pointer &&
               view_matrix && projection_matrix;
    }
};
/// The "camera" object of the sidecar (every field required, numbers or "0x" strings).
[[nodiscard]] std::optional<CameraKeys> DecodeCamera(const nlohmann::json& value);

struct Camera {
    std::array<float, 12> view{};
    std::array<float, 16> projection{};
    bool operator==(const Camera&) const = default;
};

/// Where a world point lands in the game rectangle, in [0, 1] (x right, y down).
struct Point2 {
    float x{}, y{};
};
/// Nothing when the point is behind the camera, the clip w is degenerate, anything is not finite,
/// or it lands outside the rectangle.
[[nodiscard]] std::optional<Point2> Project(const Camera& camera, const std::array<float, 3>& world,
                                            bool flip_y, float* depth = nullptr);

enum class CameraFail { None, Root, Registry, NoMain, Slot, Association, View, Dirty, Matrix };
[[nodiscard]] std::string_view CameraFailName(CameraFail fail);

/// Pure reads through `memory` (IsValidVirtualAddressRange, Read32, Read64), each one checked.
template <typename Memory>
std::optional<Camera> ReadCamera(Memory& memory, u64 base, const CameraKeys& k,
                                 CameraFail* fail = nullptr) {
    const auto failed = [&](CameraFail why) -> std::optional<Camera> {
        if (fail) *fail = why;
        return std::nullopt;
    };
    const auto r32 = [&](u64 address) -> std::optional<u32> {
        if (!address || !memory.IsValidVirtualAddressRange(address, 4)) return std::nullopt;
        return memory.Read32(address);
    };
    const auto r64 = [&](u64 address) -> std::optional<u64> {
        if (!address || !memory.IsValidVirtualAddressRange(address, 8)) return std::nullopt;
        return memory.Read64(address);
    };
    const auto ptr = [&](u64 address) -> u64 { return r64(address).value_or(0); };
    if (!k.Valid()) return failed(CameraFail::Root);
    const u64 s = ptr(base + k.registry_cell);
    const u64 t = s ? ptr(s + k.registry_table) : 0;
    const auto mask = t ? r32(t + k.mask) : std::nullopt;
    if (!mask) return failed(CameraFail::Root);
    std::optional<u32> index;
    for (u32 i = 0; i < 32 && !index; ++i) {
        if (!(*mask & (1U << i))) continue;
        const u64 b = ptr(t + k.buckets + 8 * u64{i});
        const auto capacity = b ? r32(b + k.bucket_capacity) : std::nullopt;
        const u64 hashes = b ? ptr(b + k.bucket_hashes) : 0;
        const auto count = b ? r32(b + k.bucket_count) : std::nullopt;
        if (!capacity || !hashes || !count || *count < 1 || *count > *capacity || *count > 256)
            return failed(CameraFail::Registry);
        for (u32 j = 0; j < *count; ++j) {
            const auto hash = r32(hashes + 4 * u64{j});
            if (!hash) return failed(CameraFail::Registry);
            if (*hash == k.main_hash) {
                index = r32(b + k.bucket_index);
                if (!index) return failed(CameraFail::Registry);
                break;
            }
        }
    }
    if (!index || *index >= 16) return failed(CameraFail::NoMain);
    const u64 r = ptr(base + k.assoc_cell);
    const auto slot_bits = r ? r32(r + k.assoc_slot + k.assoc_slot_stride * *index) : std::nullopt;
    const s32 slot = static_cast<s32>(slot_bits.value_or(0xFFFFFFFF));
    if (slot < 0 || slot >= 32) return failed(CameraFail::Slot);
    const u64 a = ptr(r + k.assoc_entry + k.assoc_entry_stride * static_cast<u64>(slot));
    const u64 p = a ? ptr(a + k.projection) : 0;
    if (!p) return failed(CameraFail::Association);
    const u64 d = ptr(base + k.views_cell);
    const auto views = d ? r32(d + k.views_count) : std::nullopt;
    if (!views || *index >= *views) return failed(CameraFail::View);
    const u64 array = ptr(d + k.views_array);
    const u64 l = array ? ptr(array + 8 * u64{*index}) : 0;
    const auto flags_word = l ? r32(l + k.view_flags) : std::nullopt;
    if (!flags_word) return failed(CameraFail::View);
    const u32 flags = *flags_word & 0xFFFF;
    u64 v = 0;
    if ((flags & k.special_any) != 0 && (flags & k.special_all) == k.special_all) {
        const u64 special = ptr(l + k.view_special);
        if (special) v = special + k.view_special_offset;
    }
    if (!v) v = ptr(l + k.view_pointer);
    if (!v) return failed(CameraFail::View);
    const auto dirty = r32(p + k.projection_dirty);
    if (!dirty) return failed(CameraFail::Association);
    if ((*dirty & 0xFF) == 1) return failed(CameraFail::Dirty);
    Camera camera;
    const auto load = [&](u64 at, float* out, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            const auto bits = r32(at + 4 * i);
            if (!bits) return false;
            std::memcpy(&out[i], &*bits, 4);
            if (!std::isfinite(out[i])) return false;
        }
        return true;
    };
    if (!load(v + k.view_matrix, camera.view.data(), 12) ||
        !load(p + k.projection_matrix, camera.projection.data(), 16))
        return failed(CameraFail::Matrix);
    if (fail) *fail = CameraFail::None;
    return camera;
}

/// One peer as the name tags see it (the same interpolated pose the ghost is drawn with).
struct TagPeer {
    std::string name;
    std::string banc;
    bool loading{};
    bool child{};
    std::array<float, 3> pos{};
};

/// An overlay item, positioned in [0, 1] of the game rectangle. `marker` is a diagnostic cross.
/// `distance` is the view-space depth of the head point in world units (0: unknown).
struct Tag {
    std::string text;
    Point2 at;
    bool marker{};
    float distance{};
};

/// Tags for the peers in our scene (same banc, neither side loading), name at root + head height;
/// with `crosses`, also a cross at the root and one at the head point.
[[nodiscard]] std::vector<Tag> BuildTags(const Camera& camera, const CameraKeys& keys,
                                         std::string_view local_banc, bool local_loading,
                                         const std::vector<TagPeer>& peers, bool crosses);

struct Config {
    ItemNames names;
    std::shared_ptr<const Room::Config> room;
    std::optional<CameraKeys> camera; // for the running build
    std::shared_ptr<const IconPack> icons; // EDEN_NX2_OOT_COOP_TOAST_ICONS and a valid pack
};
/// The sidecar for `build_id` (40 hex digits). A missing file gives an empty config and `why`.
[[nodiscard]] Config DecodeConfig(const nlohmann::json& document, std::string_view build_id,
                                  std::string* why = nullptr);
[[nodiscard]] std::shared_ptr<const Config> LoadConfig(std::string_view build_id,
                                                       std::string* why = nullptr);

// ---- Process-wide overlay state: the co-op session writes, the presenter reads. ----

struct Panel {
    std::vector<std::string> lines;
    u64 generation{};
};
struct Marker {
    Point2 at;
    u8 colour{};
    char label{'?'};
};

struct OverlayFrame {
    std::vector<Toast> toasts;
    size_t more{}; // toasts waiting behind the visible ones
    std::shared_ptr<const IconPack> icons;
    std::vector<Tag> tags;
    std::shared_ptr<const Panel> panel;
    std::vector<Marker> markers;
    std::shared_ptr<const Checks::Panel> checks; ///< oot_coop_checks.h
    [[nodiscard]] bool Empty() const {
        return toasts.empty() && tags.empty() && !panel && markers.empty() && !checks;
    }
};

/// Queue a toast (no-op when EDEN_NX2_OOT_COOP_TOAST is off). False when `id` was seen.
bool Notify(std::string id, std::string text);
bool Notify(std::string id, Toast toast);
/// The icon pack the presenter draws toast icons from (Reset clears it).
void PublishIcons(std::shared_ptr<const IconPack> icons);
/// The name tags for this game frame; the presenter drops them after TagLifetimeMs.
inline constexpr u64 TagLifetimeMs = 250;
void PublishTags(std::vector<Tag> tags);
void PublishMarkers(std::vector<Marker> markers);
/// Immutable panel, published only on text changes; generation never reused across resets.
void PublishPanel(std::vector<std::string> lines);
/// Session reset: toasts, ids and tags cleared.
void Reset();
/// What to draw now. Cheap when nothing is queued.
[[nodiscard]] OverlayFrame Current();

/// Per co-op session: peer names, the toast feed and the per-frame name tags. Names are written by
/// the network thread, everything else runs on the frame thread.
class SessionUx {
public:
    void Configure(std::shared_ptr<const Config> config_) {
        config = std::move(config_);
        PublishIcons(config ? config->icons : nullptr);
    }
    [[nodiscard]] const Config* Settings() const { return config.get(); }
    void SetName(const std::string& origin, std::string_view name);
    [[nodiscard]] std::string Name(std::string_view origin) const;
    /// A receiver replay returned: one toast per (origin, seq).
    void ItemReceived(std::string_view origin, u64 seq, std::string_view actor, s64 count);
    void RupeeReceived(std::string_view origin, u64 seq, s64 delta);
    /// This player's give was forwarded (EDEN_NX2_OOT_COOP_TOAST_LOCAL).
    void ItemShared(std::string_view actor, s64 count);

    /// The frame drain: read the camera, project the peers, publish the tags. `log` takes a line.
    template <typename Memory, typename Log>
    void Frame(Memory& memory, u64 base, std::string_view local_banc, bool local_loading,
               std::vector<TagPeer> peers, Log log) {
        static const bool enabled = Switch("NAMETAG");
        static const bool crosses = Switch("NAMETAG_DEBUG", false);
        ++frames;
        if (!enabled || !config || !config->camera) {
            if (!std::exchange(logged_off, true)) {
                log(enabled ? "OoT co-op: name tags off (no camera keys for this build in the sidecar)"
                            : "OoT co-op: name tags off (EDEN_NX2_OOT_COOP_NAMETAG=0)");
            }
            return;
        }
        if (peers.empty() || local_banc.empty() || local_loading) {
            if (published) PublishTags({});
            published = false;
            Status(log, 0);
            return;
        }
        for (auto& peer : peers) peer.name = Name(peer.name);
        CameraFail fail{};
        const auto camera = ReadCamera(memory, base, *config->camera, &fail);
        if (!camera) {
            ++fails[static_cast<size_t>(fail)];
            if (fail != CameraFail::Dirty && fail_lines < 8) {
                ++fail_lines;
                log(fmt::format("OoT co-op: name tag camera read failed ({})", CameraFailName(fail)));
            }
            // A dirty projection keeps the previous tags (their lifetime expires them).
            Status(log, last_count);
            return;
        }
        ++camera_ok;
        if (!std::exchange(logged_camera, true) || (crosses && camera_lines < 6 && !(*camera == last_camera))) {
            ++camera_lines;
            log(fmt::format("OoT co-op: name tag camera view=[{:.3f}] projection=[{:.3f}]",
                            fmt::join(camera->view, ", "), fmt::join(camera->projection, ", ")));
        }
        last_camera = *camera;
        auto tags = BuildTags(*camera, *config->camera, local_banc, local_loading, peers, crosses);
        last_count = 0;
        for (const auto& tag : tags) {
            if (tag.marker) continue;
            ++last_count;
            last_at = tag.at;
        }
        PublishTags(std::move(tags));
        published = true;
        Status(log, last_count);
    }

private:
    template <typename Log>
    void Status(Log& log, size_t shown) {
        if (frames - status_frame < 1800) return;
        status_frame = frames;
        log(fmt::format("OoT co-op: name tags: {} shown{} (camera ok {}, dirty {}, failed {})", shown,
                        shown ? fmt::format(" at ({:.3f}, {:.3f})", last_at.x, last_at.y) : std::string{},
                        camera_ok, fails[static_cast<size_t>(CameraFail::Dirty)],
                        frames_failed()));
    }
    [[nodiscard]] u64 frames_failed() const {
        u64 n = 0;
        for (size_t i = 0; i < fails.size(); ++i)
            if (i != static_cast<size_t>(CameraFail::Dirty) && i != 0) n += fails[i];
        return n;
    }
    std::shared_ptr<const Config> config;
    mutable std::mutex names_mutex;
    std::map<std::string, std::string, std::less<>> names;
    std::array<u64, 9> fails{};
    Camera last_camera;
    Point2 last_at;
    u64 shared{}, frames{}, status_frame{}, camera_ok{}, fail_lines{}, camera_lines{};
    size_t last_count{};
    bool logged_off{}, logged_camera{}, published{};
};

} // namespace Core::OotCoop::Ux
