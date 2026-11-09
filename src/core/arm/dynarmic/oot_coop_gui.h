// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>
#include "common/common_types.h"

namespace Core::OotCoop::Gui {
bool CoopTitle(u64 title_id);
enum class Override { Follow, On, Off };
enum class Cleanup { Follow, Death, FlagOnly };
struct Settings {
    bool enabled{};
    std::string relay = "127.0.0.1:43390", room = "default", name = "Player", team = "default";
    std::string seed = "0", keys_path, allow = "bool,bit", policy;
    Override quest{}, teleport{};
    Cleanup cleanup{};
    bool tint{};
    std::string ghost_mod = "coop-ghost";
};
std::map<std::string, std::string> ToIni(const Settings&);
Settings FromIni(const std::map<std::string, std::string>&);
bool ValidEndpoint(std::string_view);
bool ValidToken(std::string_view);
std::optional<std::set<std::string>> ParseAllow(std::string_view);
std::string FormatAllow(const std::set<std::string>&);
std::string PolicyDigest(const nlohmann::json& keyfile);
std::string PythonDumps(const nlohmann::ordered_json&);
struct Switch {
    bool applicable{};
    // Empty means not applicable; "mixed" means builds have different values.
    std::string value;
};
struct SwitchView { Switch quest, teleport, cleanup; };
SwitchView InspectSwitches(const nlohmann::ordered_json&);
std::optional<std::string> ApplyOverrides(std::string_view keyfile_text, const Settings&,
                                          std::string* why = nullptr);
using EnvGetter = std::function<std::string(std::string_view)>;
using FileReader = std::function<std::optional<std::string>(const std::filesystem::path&)>;
struct EnvPlan {
    std::vector<std::pair<std::string, std::optional<std::string>>> vars;
    std::optional<std::filesystem::path> derived_keys;
    // Private file contents; never log these.
    std::optional<std::string> derived_text;
    std::string note;
};
EnvPlan PlanEnvironment(const Settings&, const std::set<std::string>& external_names,
                        const std::filesystem::path& session_dir, const EnvGetter&,
                        const FileReader&);
bool WriteSessionKeys(const std::filesystem::path& session_dir,
                      const std::filesystem::path& original, std::string_view text);
struct FileInfo {
    std::string name;
    std::optional<std::string> sha256_12;
    u64 size{};
};
std::vector<FileInfo> DescribeKit(const std::filesystem::path& keys);
bool RequestTeleport(const std::filesystem::path& trigger, std::string_view target);
bool SetRoomWindow(const std::filesystem::path& trigger, bool shown);
bool RequestResync(const std::filesystem::path& eden_dir);
bool RoomWindowShown(const std::filesystem::path&);
std::filesystem::path RoomTrigger(const EnvGetter&);
std::filesystem::path TeleportTrigger(const EnvGetter&);
struct PeerStatus {
    std::string id, name, area;
    bool online{}, has_pose{}, child{}, loading{};
    u64 seen_ms{};
};
struct Status {
    bool live{};
    std::string relay_state;
    std::optional<u64> rtt_ms;
    std::string room, team, name, seed, policy, mod_hash, local_area;
    bool local_child{}, teleport{};
    std::vector<PeerStatus> peers;
    // Timestamps use steady_clock milliseconds since epoch, shared with the publisher.
    u64 published_ms{}, serial{};
};
void PublishStatus(Status);
Status CurrentStatus();
void ClearStatus();
} // namespace Core::OotCoop::Gui
