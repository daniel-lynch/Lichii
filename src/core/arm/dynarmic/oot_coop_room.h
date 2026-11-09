// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include "common/common_types.h"

namespace Core::OotCoop {
enum class Kind;
namespace Room {
u64 NowMs();
struct Notice { std::string id, text, log; };
void Dispatch(std::vector<Notice> notices);
struct RoomPeer {
    std::string origin, name, banc;
    bool online{}, has_pose{}, child{}, loading{};
    u64 seen{};
};
// All methods lock internally; callbacks/Notify run only after releasing the lock.
class PresenceBook {
public:
    std::vector<Notice> Receive(const nlohmann::json& packet, std::string_view name, u64 now);
    std::vector<Notice> Tick(u64 now);
    std::vector<Notice> Lost(u64 now);
    std::vector<Notice> Ready(u64 now);
    void Pose(std::string_view origin, std::string_view banc, bool child, bool loading, u64 now);
    std::vector<RoomPeer> Peers() const;
private:
    struct Peer {
        RoomPeer view;
        bool ever_online{}, toasted_online{}, toasted{}, finished{}, awaiting{};
        u64 last_toast{}, serial{};
    };
    void Transition(Peer& p, u64 now, std::vector<Notice>& out);
    void Log(Peer& p, std::string_view action, std::string_view disposition, std::vector<Notice>& out);
    mutable std::mutex mutex;
    std::map<std::string, Peer, std::less<>> peers;
    bool ready{}, ever_ready{}, lost{};
    std::optional<u64> reconcile;
    u64 relay_serial{}, log_lines{};
};

struct SyncResult {
    u64 id{}, flags{}, items{}, entries{};
    std::string Text() const;
};
class SyncTally {
public:
    void Snapshot(u64 first, u64 last); // inclusive; includes the final HighWater
    void Drained(u64 index, std::optional<Kind> kind, bool queued, u64 now);
    std::vector<SyncResult> Settle(u64 now, bool idle);
private:
    struct Range { u64 first{}, last{}; SyncResult result; std::optional<u64> drained; };
    std::mutex mutex;
    std::deque<Range> ranges;
    u64 serial{};
};
// One scope per drained entry also accounts for the caller's early continues.
class DrainEntry {
public:
    DrainEntry(SyncTally& tally_, u64 index_, std::optional<Kind> kind_, size_t before_,
               std::function<size_t()> pending_)
        : tally{tally_}, index{index_}, kind{kind_}, before{before_}, pending{std::move(pending_)} {}
    ~DrainEntry() { tally.Drained(index, kind, pending() > before, NowMs()); }
private:
    SyncTally& tally;
    u64 index;
    std::optional<Kind> kind;
    size_t before;
    std::function<size_t()> pending;
};
class FinishWatch {
public:
    bool Sample(std::optional<bool> value);
    void Rebaseline() { last.reset(); }
private:
    std::optional<bool> last;
    bool finished{};
};
std::optional<u32> DecodeFinish(const nlohmann::json& value);

struct Point2 { float x{}, y{}; bool clamped{}; };
struct MapTransform {
    std::array<float, 2> centre{0.5f, 0.5f}, half{0.1f, 0.1f}, world_centre{};
    float scale{1.0f}, yaw_sign{1.0f};
    bool circle{}, player_centre{}, player_rotation{}, swap_xz{};
    float x_sign{1.0f}, z_sign{1.0f};
};
std::optional<Point2> PlaceMarker(const MapTransform& map, const std::array<float, 3>& local,
                                float yaw_degrees, const std::array<float, 3>& peer);
std::optional<MapTransform> DecodeMinimap(const nlohmann::json& value);
// TODO(C16 minimap research): validate and read the live transform/visibility through memory.
// No guest layout is assumed. Even a configured transform stays inactive until this is wired.
template <typename Memory>
std::optional<MapTransform> ReadMinimap(Memory&, u64, const MapTransform&) { return std::nullopt; }
struct Config {
    std::optional<u32> finish;
    std::map<std::string, std::string, std::less<>> area_names;
    std::optional<MapTransform> minimap;
};
Config DecodeConfig(const nlohmann::json& document, std::string_view build, bool minimap_enabled);
std::string PrettyArea(std::string_view banc);
// The room window's area text for one peer ("-", "loading", sidecar name or prettified scene).
std::string AreaText(const RoomPeer& peer, const Config& config);
std::string Seen(u64 now, u64 then);
struct WindowInfo {
    std::string room, seed, policy, relay, name;
    std::optional<u64> rtt;
    std::optional<RoomPeer> local;
};
std::vector<std::string> WindowText(const WindowInfo& info, std::vector<RoomPeer> peers,
                                    const Config& config, u64 now);
class Window {
public:
    void Frame(const WindowInfo& info, const PresenceBook& book, const Config& config, u64 now);
private:
    bool initialized{}, shown{};
    std::string mode, trigger;
    std::optional<u64> polled, built;
    std::vector<std::string> lines;
};
} // namespace Room
} // namespace Core::OotCoop
