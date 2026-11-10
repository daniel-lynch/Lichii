// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <chrono>
#include <fstream>
#include <iterator>
#include <limits>
#include <catch2/catch_test_macros.hpp>
#include "core/arm/dynarmic/oot_coop_gui.h"

namespace {
namespace Gui = Core::OotCoop::Gui;
using Json = nlohmann::ordered_json;
constexpr auto Kit = R"({"bool":[9,2,9,1],"offsets":{"synthetic-a":{"quest":{"enabled":true},"teleport":{"enabled":false},"world":{"cleanup":"death"}},"synthetic-b":{"quest":{"enabled":true},"teleport":{"enabled":false},"world":{"cleanup":"death"}}}})";
std::optional<std::string> Read(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    return std::string{std::istreambuf_iterator<char>{in}, {}};
}
struct Scratch {
    // Test artifacts stay below the test runner's working directory, never in the user's profile.
    std::filesystem::path path = std::filesystem::current_path() /
        ("oot-coop-gui-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Scratch() { std::filesystem::create_directories(path); }
    ~Scratch() { std::error_code ec; std::filesystem::remove_all(path, ec); }
};
std::optional<std::string> Planned(const Gui::EnvPlan& plan, const std::string& name) {
    for (const auto& [key, value] : plan.vars) if (key == "EDEN_NX2_OOT_COOP" + name) return value;
    return std::nullopt;
}
bool Touches(const Gui::EnvPlan& plan, const std::string& name) {
    for (const auto& [key, value] : plan.vars) if (key == "EDEN_NX2_OOT_COOP" + name) return true;
    return false;
}
} // namespace

TEST_CASE("Co-op GUI INI defaults and round trip", "[oot_coop]") {
    Gui::Settings s;
    REQUIRE(Gui::ToIni(Gui::FromIni({})) == Gui::ToIni(s));
    s.enabled = s.tint = true;
    s.relay = "example.invalid:12345"; s.room = "Room A"; s.team = "Team A"; s.name = "Peer A";
    s.seed = "1234"; s.keys_path = "private/example.json"; s.policy = "example-policy";
    s.allow = "bool,bit,quest"; s.quest = Gui::Override::Off; s.teleport = Gui::Override::On;
    s.cleanup = Gui::Cleanup::FlagOnly; s.ghost_mod = "synthetic-mod";
    REQUIRE(Gui::ToIni(Gui::FromIni(Gui::ToIni(s))) == Gui::ToIni(s));
    auto invalid = Gui::FromIni({{"enabled", "yes"}, {"relay", "bad"}, {"room", ""},
        {"name", "\n"}, {"allow", "unknown"}, {"cleanup", "bad"}, {"quest", "bad"},
        {"ghost_mod", "../escape"}, {"unrecognized", "anything"}});
    REQUIRE(Gui::ToIni(invalid) == Gui::ToIni(Gui::Settings{}));
    REQUIRE_FALSE(Gui::CoopTitle(0));
}
TEST_CASE("Co-op GUI validates endpoints tokens and classes", "[oot_coop]") {
    for (const auto* endpoint : {"localhost:1", "127.0.0.1:65535", "[::1]:43390"})
        REQUIRE(Gui::ValidEndpoint(endpoint));
    for (const auto* endpoint : {"", ":1", "host:0", "host:65536", "host:abc", "host:1x", "host: 1", "host name:2", "host:-1"})
        REQUIRE_FALSE(Gui::ValidEndpoint(endpoint));
    REQUIRE(Gui::ValidToken("Peer A")); REQUIRE(Gui::ValidToken(std::string(64, 'a')));
    REQUIRE_FALSE(Gui::ValidToken(std::string(65, 'a'))); REQUIRE_FALSE(Gui::ValidToken("bad\n"));
    REQUIRE_FALSE(Gui::ValidToken("\x7f")); REQUIRE_FALSE(Gui::ValidToken("\xc3\xa9"));
    REQUIRE(Gui::FormatAllow(*Gui::ParseAllow("quest,bool,bit,bool")) == "bool,bit,quest");
    REQUIRE(Gui::ParseAllow("")->empty());
    REQUIRE_FALSE(Gui::ParseAllow("bool,other")); REQUIRE_FALSE(Gui::ParseAllow("bool,"));
}
TEST_CASE("Co-op GUI Python JSON serialization", "[oot_coop]") {
    const Json value = {{"z", Json::array({1, true, nullptr, "\xc3\xa9\xf0\x9f\x98\x80"})}, {"a", 0.25}};
    REQUIRE(Gui::PythonDumps(value) == R"({"z": [1, true, null, "\u00e9\ud83d\ude00"], "a": 0.25})");
    const std::vector<std::pair<double, std::string>> cases{
        {0.0, "0.0"}, {-0.0, "-0.0"}, {0.25, "0.25"}, {1e-5, "1e-05"}, {1e-4, "0.0001"},
        {1e15, "1000000000000000.0"}, {1e16, "1e+16"}, {1e20, "1e+20"},
        {1.2345678901234567, "1.2345678901234567"}, {1.0000000000000001e18, "1.0000000000000001e+18"},
        {std::numeric_limits<double>::denorm_min(), "5e-324"}};
    for (const auto& [number, expected] : cases) REQUIRE(Gui::PythonDumps(Json(number)) == expected);
    REQUIRE(Gui::PythonDumps(Json::array()) == "[]");
    REQUIRE(Gui::PythonDumps(Json::object()) == "{}");
    REQUIRE(Gui::PolicyDigest(nlohmann::json::parse(Kit)) == "flags-dc71b797646f1c7615910b9b71f79814");
    REQUIRE(Gui::PolicyDigest(nlohmann::json{{"bool", {1, 2, 9}}}) == Gui::PolicyDigest(nlohmann::json::parse(Kit)));
    REQUIRE(Gui::PolicyDigest(nlohmann::json{{"bool", {1, 2.0}}}).empty());
    REQUIRE(Gui::PolicyDigest(nlohmann::json{{"bool", {true}}}).empty());
    REQUIRE(Gui::PolicyDigest(nlohmann::json::object()).empty());
    REQUIRE(Gui::PolicyDigest(nlohmann::json::parse(
        R"({"bool":[-1,18446744073709551615,2,-1]})")) ==
        "flags-f546c30540a8a1a15481c141edaae669");
}
TEST_CASE("Co-op GUI overrides only existing typed leaves in all builds", "[oot_coop]") {
    auto file = Json::parse(Kit);
    auto view = Gui::InspectSwitches(file);
    REQUIRE(view.quest.applicable); REQUIRE(view.quest.value == "on");
    REQUIRE(view.teleport.applicable); REQUIRE(view.teleport.value == "off");
    REQUIRE(view.cleanup.value == "death");
    auto mixed = file;
    mixed["offsets"]["synthetic-b"]["quest"]["enabled"] = false;
    REQUIRE(Gui::InspectSwitches(mixed).quest.applicable);
    REQUIRE(Gui::InspectSwitches(mixed).quest.value == "mixed");
    Gui::Settings s;
    std::string why;
    REQUIRE_FALSE(Gui::ApplyOverrides(Kit, s, &why)); REQUIRE(why.empty());
    s.quest = Gui::Override::On;
    REQUIRE_FALSE(Gui::ApplyOverrides(Kit, s));
    s.quest = Gui::Override::Off; s.teleport = Gui::Override::On; s.cleanup = Gui::Cleanup::FlagOnly;
    const auto changed = Gui::ApplyOverrides(Kit, s, &why);
    REQUIRE(changed); REQUIRE(why.empty()); REQUIRE(changed->back() == '\n');
    const auto result = Json::parse(*changed);
    for (const auto& build : result["offsets"]) {
        REQUIRE(build["quest"]["enabled"] == false);
        REQUIRE(build["teleport"]["enabled"] == true);
        REQUIRE(build["world"]["cleanup"] == "flag_only");
    }
    REQUIRE(result["bool"] == file["bool"]);
    REQUIRE_FALSE(Gui::ApplyOverrides(*changed, s));
    file["offsets"]["synthetic-b"].erase("quest");
    REQUIRE_FALSE(Gui::InspectSwitches(file).quest.applicable);
    const auto partial = Gui::ApplyOverrides(file.dump(), s);
    REQUIRE(partial);
    REQUIRE_FALSE(Json::parse(*partial)["offsets"]["synthetic-b"].contains("quest"));
    file["offsets"]["synthetic-a"]["quest"]["enabled"] = 1;
    REQUIRE_FALSE(Gui::ApplyOverrides(file.dump(), s, &why)); REQUIRE_FALSE(why.empty());
    REQUIRE_FALSE(Gui::ApplyOverrides("{", s, &why)); REQUIRE_FALSE(why.empty());
    REQUIRE_FALSE(Gui::InspectSwitches(Json{{"offsets", Json::object()}}).quest.applicable);
}
TEST_CASE("Co-op GUI environment planning preserves external names", "[oot_coop]") {
    Gui::Settings s;
    s.enabled = true; s.keys_path = "kit/synthetic.json";
    std::map<std::string, std::string> env{{"EDEN_NX2_OOT_COOP_ROOM", "launcher-room"}};
    const auto getter = [&](std::string_view key) { return env[std::string(key)]; };
    const auto reader = [](const std::filesystem::path&) -> std::optional<std::string> { return Kit; };
    const auto plan = Gui::PlanEnvironment(s, {"EDEN_NX2_OOT_COOP_ROOM"}, "session", getter, reader);
    REQUIRE_FALSE(Touches(plan, "_ROOM")); REQUIRE(Planned(plan, "") == "1");
    REQUIRE(Planned(plan, "_KEYS") == s.keys_path);
    REQUIRE(Planned(plan, "_POLICY") == "flags-dc71b797646f1c7615910b9b71f79814");
    REQUIRE_FALSE(plan.derived_keys); REQUIRE_FALSE(Planned(plan, "_TINT"));
    REQUIRE(std::filesystem::path(*Planned(plan, "_TELEPORT_TRIGGER")) == std::filesystem::path("kit/coop-teleport.trigger"));
    s.quest = Gui::Override::Off;
    const auto derived = Gui::PlanEnvironment(s, {}, "session", getter, reader);
    REQUIRE(derived.derived_keys == std::filesystem::path("session/synthetic.json"));
    REQUIRE(derived.derived_text);
    REQUIRE(Planned(derived, "_POLICY") == Gui::PolicyDigest(Json::parse(*derived.derived_text)));
    REQUIRE(std::filesystem::path(*Planned(derived, "_TELEPORT_TRIGGER")) == std::filesystem::path("session/coop-teleport.trigger"));
    env["EDEN_NX2_OOT_COOP_KEYS"] = "external/other.json";
    std::filesystem::path read_path;
    const auto external_reader = [&](const std::filesystem::path& path) -> std::optional<std::string> {
        read_path = path; return R"({"bool":[42]})";
    };
    const auto external = Gui::PlanEnvironment(s, {"EDEN_NX2_OOT_COOP_KEYS", "EDEN_NX2_OOT_COOP_POLICY"},
        "session", getter, external_reader);
    REQUIRE(read_path == std::filesystem::path("external/other.json"));
    REQUIRE_FALSE(external.derived_keys); REQUIRE_FALSE(Touches(external, "_KEYS"));
    REQUIRE_FALSE(Touches(external, "_POLICY"));
    const auto auto_policy = Gui::PlanEnvironment(s, {"EDEN_NX2_OOT_COOP_KEYS"}, "session", getter, external_reader);
    REQUIRE(Planned(auto_policy, "_POLICY") == Gui::PolicyDigest(nlohmann::json{{"bool", {42}}}));
    s.enabled = false;
    const auto disabled = Gui::PlanEnvironment(s, {"EDEN_NX2_OOT_COOP_KEYS", "EDEN_NX2_OOT_COOP"}, "session", getter, reader);
    REQUIRE_FALSE(Touches(disabled, "")); REQUIRE_FALSE(Touches(disabled, "_KEYS"));
    REQUIRE_FALSE(Planned(disabled, "_ROOM"));
    REQUIRE(std::filesystem::path(*Planned(disabled, "_TELEPORT_TRIGGER")) == std::filesystem::path("external/coop-teleport.trigger"));
    const auto preserved_trigger = Gui::PlanEnvironment(s,
        {"EDEN_NX2_OOT_COOP_TELEPORT_TRIGGER"}, "session", getter, reader);
    REQUIRE_FALSE(Touches(preserved_trigger, "_TELEPORT_TRIGGER"));
    const auto all_off = Gui::PlanEnvironment(s, {}, "session", getter, reader);
    for (const auto& [key, value] : all_off.vars) REQUIRE_FALSE(value);
}
TEST_CASE("Co-op GUI session kit copies sidecars and removes stale files", "[oot_coop]") {
    Scratch scratch;
    const auto source = scratch.path / "source";
    const auto session = scratch.path / "session";
    std::filesystem::create_directory(source);
    const auto keys = source / "synthetic.json";
    std::ofstream(keys) << Kit;
    for (const auto* sidecar : {"coop-ux.json", "coop-anim.json", "coop-equip.json", "coop-ledger.json"})
        std::ofstream(source / sidecar) << "{}";
    REQUIRE(Gui::WriteSessionKeys(session, keys, "{\"synthetic\": true}\n"));
    REQUIRE(Read(keys) == Kit); REQUIRE(Read(session / keys.filename()) == "{\"synthetic\": true}\n");
    REQUIRE(Gui::DescribeKit(session / keys.filename()).size() == 5);
    for (const auto& info : Gui::DescribeKit(keys)) {
        REQUIRE(info.sha256_12); REQUIRE(info.sha256_12->size() == 12); REQUIRE(info.size > 0);
    }
    std::ofstream(source / "coop-ux.json") << "{\"changed\": true}";
    std::filesystem::remove(source / "coop-anim.json");
    REQUIRE(Gui::WriteSessionKeys(session, keys, "{}"));
    REQUIRE(Read(session / "coop-ux.json") == "{\"changed\": true}");
    REQUIRE_FALSE(std::filesystem::exists(session / "coop-anim.json"));
    // The icon pack directory is mirrored too (copied when present, removed when gone).
    std::filesystem::create_directory(source / "coop-icons");
    std::ofstream(source / "coop-icons" / "index.json") << "{}";
    REQUIRE(Gui::WriteSessionKeys(session, keys, "{}"));
    REQUIRE(Read(session / "coop-icons" / "index.json") == "{}");
    std::filesystem::remove_all(source / "coop-icons");
    REQUIRE(Gui::WriteSessionKeys(session, keys, "{}"));
    REQUIRE_FALSE(std::filesystem::exists(session / "coop-icons"));
    REQUIRE_FALSE(Gui::WriteSessionKeys(source, keys, "must not overwrite original"));
    REQUIRE(Read(keys) == Kit);
}
TEST_CASE("Co-op GUI file triggers validate and append", "[oot_coop]") {
    Scratch scratch;
    const auto teleport = scratch.path / "teleport";
    REQUIRE(Gui::RequestTeleport(teleport, "peer_1.A-2")); REQUIRE(Gui::RequestTeleport(teleport, "any"));
    for (const auto* target : {"", "bad name", "bad\nname", "bad/path", "\xc3\xa9"})
        REQUIRE_FALSE(Gui::RequestTeleport(teleport, target));
    REQUIRE_FALSE(Gui::RequestTeleport(teleport, std::string(65, 'a')));
    REQUIRE(Read(teleport) == "teleport peer_1.A-2\nteleport any\n");
    const auto room = scratch.path / "room";
    REQUIRE(Gui::SetRoomWindow(room, true)); REQUIRE(Gui::RoomWindowShown(room));
    REQUIRE(Gui::SetRoomWindow(room, false)); REQUIRE_FALSE(Gui::RoomWindowShown(room));
    REQUIRE(Gui::SetRoomWindow(room, false)); REQUIRE(Gui::RequestResync(scratch.path));
    REQUIRE(std::filesystem::exists(scratch.path / "coop-resync"));
    std::map<std::string, std::string> env;
    const auto getter = [&](std::string_view key) { return env[std::string(key)]; };
    REQUIRE(Gui::RoomTrigger(getter).empty()); REQUIRE(Gui::TeleportTrigger(getter).empty());
    env["EDEN_NX2_OOT_COOP_KEYS"] = "kit/key.json";
    REQUIRE(Gui::RoomTrigger(getter) == std::filesystem::path("kit/coop-room.show"));
    REQUIRE(Gui::TeleportTrigger(getter).empty());
    env["EDEN_NX2_OOT_COOP_ROOM_TRIGGER"] = "custom.show";
    env["EDEN_NX2_OOT_COOP_TELEPORT_TRIGGER"] = "custom.teleport";
    REQUIRE(Gui::RoomTrigger(getter) == "custom.show"); REQUIRE(Gui::TeleportTrigger(getter) == "custom.teleport");
}
TEST_CASE("Co-op GUI status publication copies and advances serial", "[oot_coop]") {
    Gui::ClearStatus();
    const auto first = Gui::CurrentStatus();
    REQUIRE_FALSE(first.live);
    Gui::Status next;
    next.live = true; next.room = "synthetic-room"; next.published_ms = 1000;
    next.peers.push_back({.id = "synthetic-id", .name = "Peer", .online = true});
    Gui::PublishStatus(next);
    auto current = Gui::CurrentStatus();
    REQUIRE(current.serial > first.serial); REQUIRE(current.room == next.room);
    current.peers.clear(); REQUIRE(Gui::CurrentStatus().peers.size() == 1);
    Gui::PublishStatus(next); REQUIRE(Gui::CurrentStatus().serial > current.serial);
    const auto before_clear = Gui::CurrentStatus().serial;
    Gui::ClearStatus(); REQUIRE_FALSE(Gui::CurrentStatus().live);
    REQUIRE(Gui::CurrentStatus().serial > before_clear); REQUIRE(Gui::CurrentStatus().peers.empty());
}

TEST_CASE("Co-op GUI disconnect and connect use a persistent trigger", "[oot_coop]") {
    Scratch scratch;
    REQUIRE_FALSE(Gui::DisconnectRequested(scratch.path));
    REQUIRE(Gui::RequestConnect(scratch.path));
    REQUIRE(Gui::RequestDisconnect(scratch.path));
    REQUIRE(Gui::RequestDisconnect(scratch.path));
    REQUIRE(Gui::DisconnectRequested(scratch.path));
    REQUIRE(Gui::RequestConnect(scratch.path));
    REQUIRE_FALSE(Gui::DisconnectRequested(scratch.path));
    REQUIRE(Gui::RequestConnect(scratch.path));
    REQUIRE_FALSE(Gui::RequestConnect({}));
    REQUIRE_FALSE(Gui::RequestDisconnect({}));
    std::filesystem::create_directory(scratch.path / "coop-disconnect");
    std::ofstream(scratch.path / "coop-disconnect/child") << "synthetic";
    REQUIRE(Gui::DisconnectRequested(scratch.path));
    REQUIRE_FALSE(Gui::RequestConnect(scratch.path));
    REQUIRE_FALSE(Gui::RequestDisconnect(scratch.path));
}
