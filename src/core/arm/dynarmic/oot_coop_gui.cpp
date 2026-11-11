// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/arm/dynarmic/oot_coop_gui.h"
#include "core/arm/dynarmic/oot_coop_link.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iterator>
#include <mutex>
#include <openssl/evp.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include "core/arm/dynarmic/oot_gamedata_trace.h"
#include "common/fs/fs_util.h"

namespace Core::OotCoop::Gui {
namespace {
using Json = nlohmann::ordered_json;
constexpr std::array Tokens{"bool", "bit", "rupee", "item", "pose", "dungeon", "quest"};
constexpr std::array Sidecars{"coop-ux.json", "coop-anim.json", "coop-equip.json", "coop-ledger.json"};
constexpr const char* IconPackDir = "coop-icons";
constexpr std::string_view Prefix = "EDEN_NX2_OOT_COOP";
std::mutex status_mutex;
Status status;
u64 serial{};

std::string Hash(std::string_view bytes) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int size{};
    if (EVP_Digest(bytes.data(), bytes.size(), digest, &size, EVP_sha256(), nullptr) != 1)
        return {};
    constexpr char Hex[] = "0123456789abcdef";
    std::string out;
    for (unsigned int i = 0; i < size; ++i) {
        out += Hex[digest[i] >> 4];
        out += Hex[digest[i] & 15];
    }
    return out;
}
std::optional<std::string> Read(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::nullopt;
    std::string bytes{std::istreambuf_iterator<char>{in}, {}};
    if (in.bad()) return std::nullopt;
    return bytes;
}
std::string EnvName(std::string_view suffix) { return std::string{Prefix} + std::string{suffix}; }
const Json* Leaf(const Json& build, const char* section, const char* leaf) {
    if (!build.is_object() || !build.contains(section) || !build[section].is_object() ||
        !build[section].contains(leaf)) return nullptr;
    return &build[section][leaf];
}
bool RightType(const Json& value, bool cleanup) {
    return cleanup ? value.is_string() && (value == "death" || value == "flag_only")
                   : value.is_boolean();
}
Switch Inspect(const Json& file, const char* section, const char* leaf, bool cleanup) {
    Switch result;
    if (!file.is_object() || !file.contains("offsets") || !file["offsets"].is_object() ||
        file["offsets"].empty()) return result;
    for (const auto& build : file["offsets"]) {
        const auto* value = Leaf(build, section, leaf);
        if (!value || !RightType(*value, cleanup)) return {};
        const std::string text = cleanup ? value->get<std::string>() : (*value == true ? "on" : "off");
        if (!result.applicable) result.value = text;
        else if (result.value != text) result.value = "mixed";
        result.applicable = true;
    }
    return result;
}

// Python uses shortest round-trip digits, fixed notation for exponents [-4, 16),
// and a signed, at least two-digit exponent otherwise.
std::string PythonFloat(double value) {
    if (std::isnan(value)) return "NaN";
    if (std::isinf(value)) return value < 0 ? "-Infinity" : "Infinity";
    std::array<char, 64> buf{};
    const auto [end, error] = std::to_chars(buf.data(), buf.data() + buf.size(), value,
                                           std::chars_format::scientific);
    if (error != std::errc{}) return {};
    std::string text(buf.data(), end);
    const auto e = text.find('e');
    int exponent{};
    auto exp = std::string_view{text}.substr(e + 1);
    if (exp.starts_with('+')) exp.remove_prefix(1);
    std::from_chars(exp.data(), exp.data() + exp.size(), exponent);
    std::string digits = text.substr(0, e);
    const bool negative = digits.starts_with('-');
    if (negative) digits.erase(0, 1);
    std::erase(digits, '.');
    const std::string sign = negative ? "-" : "";
    if (exponent < -4 || exponent >= 16) {
        std::string mantissa(1, digits[0]);
        if (digits.size() > 1) mantissa += "." + digits.substr(1);
        auto power = std::to_string(std::abs(exponent));
        if (power.size() < 2) power.insert(0, "0");
        return sign + mantissa + (exponent < 0 ? "e-" : "e+") + power;
    }
    const int point = exponent + 1;
    if (point <= 0) return sign + "0." + std::string(-point, '0') + digits;
    if (static_cast<size_t>(point) >= digits.size())
        return sign + digits + std::string(point - digits.size(), '0') + ".0";
    digits.insert(point, ".");
    return sign + digits;
}
} // namespace

bool CoopTitle(u64 title_id) { return title_id == OotGameDataTrace::ProgramId; }
bool ValidToken(std::string_view text) {
    return !text.empty() && text.size() <= 64 &&
           std::all_of(text.begin(), text.end(), [](unsigned char c) { return c >= 32 && c <= 126; });
}
bool ValidEndpoint(std::string_view text) {
    const auto colon = text.rfind(':');
    if (colon == text.npos || colon == 0 || colon + 1 == text.size() ||
        !std::all_of(text.begin(), text.end(), [](unsigned char c) { return c > 32 && c < 127; }))
        return false;
    unsigned port{};
    const auto [end, ec] = std::from_chars(text.data() + colon + 1, text.data() + text.size(), port);
    return ec == std::errc{} && end == text.data() + text.size() && port > 0 && port <= 65535;
}
std::optional<std::set<std::string>> ParseAllow(std::string_view text) {
    std::set<std::string> result;
    if (text.empty()) return result;
    while (true) {
        const auto comma = text.find(',');
        const auto token = text.substr(0, comma);
        if (std::find(Tokens.begin(), Tokens.end(), token) == Tokens.end()) return std::nullopt;
        result.emplace(token);
        if (comma == text.npos) return result;
        text.remove_prefix(comma + 1);
    }
}
std::string FormatAllow(const std::set<std::string>& tokens) {
    std::string result;
    for (const auto* token : Tokens) {
        if (!tokens.contains(token)) continue;
        if (!result.empty()) result += ',';
        result += token;
    }
    return result;
}
std::map<std::string, std::string> ToIni(const Settings& s) {
    const auto over = [](Override v) { return v == Override::On ? "on" : v == Override::Off ? "off" : "follow"; };
    return {{"enabled", s.enabled ? "1" : "0"}, {"relay", s.relay}, {"room", s.room},
            {"name", s.name}, {"team", s.team}, {"seed", s.seed}, {"keys_path", s.keys_path},
            {"allow", s.allow}, {"policy", s.policy}, {"quest", over(s.quest)},
            {"teleport", over(s.teleport)}, {"cleanup", s.cleanup == Cleanup::Death ? "death" :
                s.cleanup == Cleanup::FlagOnly ? "flag_only" : "follow"},
            {"tint", s.tint ? "1" : "0"}, {"ghost_mod", s.ghost_mod}};
}
Settings FromIni(const std::map<std::string, std::string>& values) {
    Settings s;
    const auto get = [&](const char* key, std::string& out, auto valid) {
        const auto it = values.find(key);
        if (it != values.end() && valid(it->second)) out = it->second;
    };
    const auto text = [](const std::string& v) { return v.find('\0') == v.npos; };
    get("relay", s.relay, ValidEndpoint);
    get("room", s.room, ValidToken); get("team", s.team, ValidToken); get("name", s.name, ValidToken);
    get("seed", s.seed, ValidToken); get("policy", s.policy, text); get("keys_path", s.keys_path, text);
    get("ghost_mod", s.ghost_mod, [](const std::string& v) {
        return ValidToken(v) && v != "." && v != ".." && v.find_first_of("/\\") == v.npos;
    });
    get("allow", s.allow, [](const std::string& v) { return ParseAllow(v).has_value(); });
    s.allow = FormatAllow(*ParseAllow(s.allow));
    const auto value = [&](const char* key) { const auto it = values.find(key); return it == values.end() ? "" : it->second; };
    s.enabled = value("enabled") == "1"; s.tint = value("tint") == "1";
    const auto over = [&](const char* key) {
        const auto v = value(key);
        return v == "on" ? Override::On : v == "off" ? Override::Off : Override::Follow;
    };
    s.quest = over("quest"); s.teleport = over("teleport");
    s.cleanup = value("cleanup") == "death" ? Cleanup::Death : value("cleanup") == "flag_only" ? Cleanup::FlagOnly : Cleanup::Follow;
    return s;
}
std::string PythonDumps(const Json& value) {
    if (value.is_number_float()) return PythonFloat(value.get<double>());
    if (!value.is_structured()) return value.dump(-1, ' ', true);
    const bool object = value.is_object();
    std::string out = object ? "{" : "[";
    bool first = true;
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (!first) out += ", ";
        first = false;
        if (object) out += Json(it.key()).dump(-1, ' ', true) + ": ";
        out += PythonDumps(it.value());
    }
    return out + (object ? "}" : "]");
}
std::string PolicyDigest(const nlohmann::json& file) {
    if (!file.is_object() || !file.contains("bool") || !file["bool"].is_array()) return {};
    std::vector<nlohmann::json> ints;
    for (const auto& value : file["bool"]) {
        if (!value.is_number_integer()) return {};
        ints.push_back(value);
    }
    const auto less = [](const nlohmann::json& a, const nlohmann::json& b) {
        const bool a_negative = !a.is_number_unsigned() && a.get<s64>() < 0;
        const bool b_negative = !b.is_number_unsigned() && b.get<s64>() < 0;
        if (a_negative != b_negative) return a_negative;
        return a_negative ? a.get<s64>() < b.get<s64>() : a.get<u64>() < b.get<u64>();
    };
    std::sort(ints.begin(), ints.end(), less);
    ints.erase(std::unique(ints.begin(), ints.end(), [&](const auto& a, const auto& b) {
        return !less(a, b) && !less(b, a);
    }), ints.end());
    const auto digest = Hash(PythonDumps(Json{{"bool", ints}}));
    return digest.empty() ? "" : "flags-" + digest.substr(0, 32);
}
SwitchView InspectSwitches(const Json& file) {
    return {Inspect(file, "quest", "enabled", false), Inspect(file, "teleport", "enabled", false),
            Inspect(file, "world", "cleanup", true)};
}
std::optional<std::string> ApplyOverrides(std::string_view text, const Settings& s, std::string* why) {
    if (why) why->clear();
    try {
        auto file = Json::parse(text);
        if (!file.is_object() || !file.contains("offsets") || !file["offsets"].is_object()) {
            if (why) *why = "Key file has no offsets object";
            return std::nullopt;
        }
        bool changed = false;
        for (auto& build : file["offsets"]) {
            if (!build.is_object()) {
                if (why) *why = "Key file build is not an object";
                return std::nullopt;
            }
            const auto set = [&](const char* section, const char* leaf, bool cleanup,
                                 bool follow, const Json& desired) {
                if (build.contains(section) && !build[section].is_object()) return false;
                const auto* current = Leaf(build, section, leaf);
                if (!current) return true;
                if (!RightType(*current, cleanup)) return false;
                if (!follow && *current != desired) {
                    build[section][leaf] = desired;
                    changed = true;
                }
                return true;
            };
            if (!set("quest", "enabled", false, s.quest == Override::Follow, s.quest == Override::On) ||
                !set("teleport", "enabled", false, s.teleport == Override::Follow, s.teleport == Override::On) ||
                !set("world", "cleanup", true, s.cleanup == Cleanup::Follow,
                     s.cleanup == Cleanup::Death ? "death" : "flag_only")) {
                if (why) *why = "Key file switch has an invalid type or value";
                return std::nullopt;
            }
        }
        if (changed) return PythonDumps(file) + "\n";
    } catch (const Json::exception&) {
        if (why) *why = "Cannot parse key file JSON";
    }
    return std::nullopt;
}
EnvPlan PlanEnvironment(const Settings& s, const std::set<std::string>& external,
                        const std::filesystem::path& session, const EnvGetter& env,
                        const FileReader& read) {
    EnvPlan plan;
    const auto put = [&](std::string_view suffix, std::optional<std::string> value) {
        const auto name = EnvName(suffix);
        if (!external.contains(name)) plan.vars.emplace_back(name, std::move(value));
    };
    const bool external_keys = external.contains(EnvName("_KEYS"));
    std::string keys = external_keys ? env(EnvName("_KEYS")) : s.enabled ? s.keys_path : "";
    if (s.enabled) {
        auto contents = keys.empty() ? std::nullopt : read(std::filesystem::path(Common::FS::ToU8String(keys)));
        if (contents && !external_keys) {
            plan.derived_text = ApplyOverrides(*contents, s, &plan.note);
            if (plan.derived_text) {
                plan.derived_keys = session / std::filesystem::path(Common::FS::ToU8String(keys)).filename();
                const auto utf8 = plan.derived_keys->u8string();
                keys.assign(utf8.begin(), utf8.end());
                contents = plan.derived_text;
            }
        } else if (!contents && !keys.empty()) {
            plan.note = "Cannot read key file";
        }
        std::string policy = s.policy;
        if (policy.empty() && contents) {
            const auto file = nlohmann::json::parse(*contents, nullptr, false);
            policy = PolicyDigest(file);
            if (policy.empty() && plan.note.empty()) plan.note = "Key file has no valid policy bool list";
        }
        put("", "1"); put("_RELAY", s.relay); put("_ROOM", s.room); put("_NAME", s.name);
        put("_TEAM", s.team); put("_SEED", s.seed); put("_ALLOW", s.allow);
        put("_TINT", s.tint ? std::optional<std::string>{"1"} : std::nullopt);
        put("_KEYS", keys.empty() ? std::nullopt : std::optional{keys});
        put("_POLICY", policy.empty() ? std::nullopt : std::optional{policy});
    } else {
        for (const auto suffix : {"", "_RELAY", "_ROOM", "_NAME", "_TEAM", "_SEED", "_ALLOW",
                                  "_TINT", "_KEYS", "_POLICY"}) put(suffix, std::nullopt);
    }
    std::optional<std::string> trigger;
    if (!keys.empty()) {
        const auto path = (std::filesystem::path(Common::FS::ToU8String(keys)).parent_path() / "coop-teleport.trigger").u8string();
        trigger = std::string(path.begin(), path.end());
    }
    put("_TELEPORT_TRIGGER", trigger);
    return plan;
}

bool WriteSessionKeys(const std::filesystem::path& session, const std::filesystem::path& original,
                      std::string_view text) {
    std::error_code ec;
    const auto source_dir = original.has_parent_path() ? original.parent_path() : std::filesystem::path{"."};
    const auto canonical_source = std::filesystem::weakly_canonical(source_dir, ec);
    if (ec) return false;
    const auto canonical_session = std::filesystem::weakly_canonical(session, ec);
    if (ec || canonical_source == canonical_session || original.filename().empty()) return false;
    std::filesystem::create_directories(session, ec);
    if (ec) return false;
    // Prepare sidecars before publishing the key file. A failed preparation must not be used.
    for (const auto* name : Sidecars) {
        const auto source = source_dir / name;
        const auto dest = session / name;
        const bool exists = std::filesystem::exists(source, ec);
        if (ec) return false;
        if (exists) std::filesystem::copy_file(source, dest, std::filesystem::copy_options::overwrite_existing, ec);
        else std::filesystem::remove(dest, ec);
        if (ec) return false;
    }
    // The optional toast icon pack (a directory; not part of the mod hash) follows the sidecars.
    {
        const auto source = source_dir / IconPackDir, dest = session / IconPackDir;
        std::filesystem::remove_all(dest, ec);
        if (ec) return false;
        if (std::filesystem::is_directory(source, ec)) {
            std::filesystem::copy(source, dest, std::filesystem::copy_options::recursive, ec);
            if (ec) return false;
        }
        ec.clear();
    }
    const auto dest = session / original.filename();
    auto temp = dest;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
        if (!out) { out.close(); std::filesystem::remove(temp, ec); return false; }
        out.close();
        if (!out) { std::filesystem::remove(temp, ec); return false; }
    }
#ifdef _WIN32
    const bool success = MoveFileExW(temp.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
    std::filesystem::rename(temp, dest, ec);
    const bool success = !ec;
#endif
    if (!success) std::filesystem::remove(temp, ec);
    return success;
}
std::vector<FileInfo> DescribeKit(const std::filesystem::path& keys) {
    std::vector<FileInfo> result;
    if (keys.empty()) return result;
    const auto add = [&](const std::filesystem::path& file) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(file, ec)) return;
        const auto size = std::filesystem::file_size(file, ec);
        if (ec) return;
        FileInfo info{Common::FS::PathToUTF8String(file.filename()), std::nullopt, size};
        if (const auto bytes = Read(file)) {
            const auto hash = Hash(*bytes);
            if (!hash.empty()) info.sha256_12 = hash.substr(0, 12);
        }
        result.push_back(std::move(info));
    };
    add(keys);
    for (const auto* sidecar : Sidecars) add(keys.parent_path() / sidecar);
    return result;
}
bool RequestTeleport(const std::filesystem::path& path, std::string_view target) {
    if (path.empty() || target.empty() || target.size() > 64 ||
        !std::all_of(target.begin(), target.end(), [](unsigned char c) {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                   c == '_' || c == '.' || c == '-';
        })) return false;
    std::ofstream out(path, std::ios::binary | std::ios::app);
    out << "teleport " << target << '\n';
    out.flush();
    return static_cast<bool>(out);
}
bool SetRoomWindow(const std::filesystem::path& path, bool shown) {
    if (path.empty()) return false;
    if (shown) {
        std::ofstream out(path, std::ios::binary | std::ios::app);
        out.flush();
        return static_cast<bool>(out);
    }
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return !ec;
}
bool RequestDisconnect(const std::filesystem::path& eden_dir) {
    return !eden_dir.empty() && SetRoomWindow(eden_dir / "coop-disconnect", true);
}
bool RequestConnect(const std::filesystem::path& eden_dir) {
    if (eden_dir.empty()) return false;
    return Link::DisconnectTrigger(eden_dir).TakeStale() != Link::DisconnectTrigger::Stale::Failed;
}
bool DisconnectRequested(const std::filesystem::path& eden_dir) {
    return !eden_dir.empty() && Link::DisconnectTrigger(eden_dir).Present();
}
bool RequestResync(const std::filesystem::path& eden_dir) {
    return !eden_dir.empty() && SetRoomWindow(eden_dir / "coop-resync", true);
}
bool RoomWindowShown(const std::filesystem::path& path) {
    std::error_code ec;
    return !path.empty() && std::filesystem::exists(path, ec);
}
std::filesystem::path RoomTrigger(const EnvGetter& env) {
    const auto explicit_path = env(EnvName("_ROOM_TRIGGER"));
    if (!explicit_path.empty()) return std::filesystem::path(Common::FS::ToU8String(explicit_path));
    const auto keys = env(EnvName("_KEYS"));
    return keys.empty() ? std::filesystem::path{} : std::filesystem::path(Common::FS::ToU8String(keys)).parent_path() / "coop-room.show";
}
std::filesystem::path TeleportTrigger(const EnvGetter& env) {
    return std::filesystem::path(Common::FS::ToU8String(env(EnvName("_TELEPORT_TRIGGER"))));
}
void PublishStatus(Status next) {
    std::scoped_lock lock{status_mutex};
    next.serial = ++serial;
    status = std::move(next);
}
Status CurrentStatus() {
    std::scoped_lock lock{status_mutex};
    return status;
}
void ClearStatus() {
    std::scoped_lock lock{status_mutex};
    status = {};
    status.serial = ++serial;
}
} // namespace Core::OotCoop::Gui
