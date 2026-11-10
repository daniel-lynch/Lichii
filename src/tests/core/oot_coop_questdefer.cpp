// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include "core/arm/dynarmic/oot_coop_questdefer.h"

namespace {
namespace Q = Core::OotCoop::Quest;
namespace D = Core::OotCoop::QuestDefer;
using Json = nlohmann::json;
Json Document() {
    return Json::parse(R"({
      "manager":{"cell":"0x100","got_cell":"0x108","dereferences":1,"got_dereferences":2,
        "tables":[{"count":16,"data":24},{"count":32,"data":40},{"count":48,"data":56}],
        "record_stride":512,"initialized":0,"busy":1,"restoring":2,"suppressed":3,"dependency_gate":4,"rollback_seen":5},
      "entries":{"setter":{"at":"0x200","words":["0xd503201f"]}},
      "commit_observation":[{"at":"0x240","words":["0xd503237f","0xd503201f"],"resume":"pacibsp",
        "state_register":0,"record_from_state":"-0x40"}],
      "layout":{
        "record":{"challenge":0,"active_condition":4,"current_hash":8,"flags_u16":12,"state_embedded":64,
          "restoration_mask":2,"pending_mask":4},
        "state":{"record_backlink":0,"initial_dependency":8,"current_index":12,"step_count":16,"step_array":24,"enum_key":32,"pending_hash":36},
        "step":{"stride":96,"hash":0,"force_progress_condition":4,"is_minigame":8,"is_boss_battle":9,
          "stop_egg_hatch":10,"safety_setting_hash":12,"participant_count":16,"participants":24}},
      "setter_arguments":{"w3":1,"w4":0,"w5":0},
      "notready_hash":11,"ready_hash":22,"complete_hash":33,
      "mirror":{"0x65":[22,44,66]}
    })");
}
Json BundleDocument() {
    auto j = Document();
    j["enabled"] = true;
    j["activity"] = {{"boss_bool", 700}, {"death_commands", {"Down_A", "Down_B"}}};
    j["bosses"] = Json::array({{{"challenge", 201}, {"enabled", true}, {"name", "Guardian A"},
        {"trigger", 55}, {"apply", 33}, {"chain", Json::array({{{"challenge", 202}, {"step", 44}}})},
        {"bools", {701, 702}}, {"region", {"Arena_A"}}}});
    return j;
}
std::shared_ptr<Q::Keys> Keys(Json j = BundleDocument()) {
    const auto decoded = Q::DecodeKeys(j);
    REQUIRE(decoded);
    auto k = std::make_shared<Q::Keys>(*decoded);
    k->apply_timeout = 3;
    return k;
}
Q::Record Record(u32 challenge, u32 index = 1) {
    Q::Record r{challenge, index, 0, 0, 0, {{11}, {22}, {44}, {55}, {66}, {33}}};
    r.current = r.steps.at(index).hash;
    return r;
}
struct Rig {
    std::shared_ptr<Q::Keys> keys;
    D::Machine machine;
    std::map<u32, Q::Record> records{{201, Record(201)}, {202, Record(202)}};
    std::map<u32, bool> bools{{701, false}, {702, false}};
    std::vector<Q::Target> calls;
    std::vector<bool> bundle_calls;
    std::vector<u32> grants;
    std::vector<std::string> logs, toasts;
    D::Observation observation{std::string{"Arena_A"}, std::string{"Live_A"}, false, false, false, true, true};
    D::Port port;
    u64 frame{};
    bool step_ok{true}, bool_ok{true};
    explicit Rig(Json j = BundleDocument(), std::set<u64> denied = {})
        : keys{Keys(std::move(j))}, machine{keys, std::move(denied)} {
        port.read = [&](u32 c) -> std::optional<Q::Record> {
            const auto it = records.find(c); return it == records.end() ? std::nullopt : std::optional{it->second};
        };
        port.step = [&](const Q::Target& t, bool bundle) {
            if (!step_ok) return false;
            calls.push_back(t); bundle_calls.push_back(bundle); return true;
        };
        port.read_bool = [&](u32 h) -> std::optional<bool> {
            const auto it = bools.find(h); return it == bools.end() ? std::nullopt : std::optional{it->second};
        };
        port.grant_bool = [&](u32 h) { if (!bool_ok) return false; grants.push_back(h); return true; };
        port.log = [&](std::string_view s) { logs.emplace_back(s); };
        port.toast = [&](std::string_view s) { toasts.emplace_back(s); };
        port.peer = [](std::string_view) { return std::string{"Peer A"}; };
        machine.Queue({201, 55, 3, "peer-a"});
    }
    bool Tick(bool available = true) {
        machine.Observe(observation, ++frame, port);
        return machine.Frame(port, frame, available);
    }
    void Frames(unsigned n, bool available = true) { while (n--) Tick(available); }
    void Commit() {
        REQUIRE_FALSE(calls.empty());
        const auto& t = calls.back();
        records.at(t.challenge) = Record(t.challenge, t.index);
        machine.Returned();
    }
    bool Logged(std::string_view text) const {
        return std::any_of(logs.begin(), logs.end(), [&](const auto& s) { return s.find(text) != std::string::npos; });
    }
};
}

TEST_CASE("questdefer optional configuration preserves C16 defaults", "[oot_coop]") {
    const auto old = Q::DecodeKeys(Document()); REQUIRE(old); REQUIRE(old->defer);
    REQUIRE(old->defer->bosses.empty()); REQUIRE(old->defer->defer_in_activity);
    REQUIRE(old->defer->death_skips); REQUIRE_FALSE(old->defer->skip_in_place);
    REQUIRE_FALSE(old->defer->during_gameover);
    const auto k = D::Decode(BundleDocument()); REQUIRE(k);
    REQUIRE(k->death_load_frames == 900); REQUIRE(k->exit_settle_frames == 60);
    REQUIRE(k->bosses.size() == 1); REQUIRE(k->Find(201));
    for (auto mode : {"after_respawn", "during_gameover"}) {
        auto j = BundleDocument(); j["death_apply"] = mode;
        REQUIRE(Q::DecodeKeys(j)); REQUIRE(D::Decode(j));
    }
    auto j = BundleDocument(); j["bosses"] = Json::array(); j.erase("activity");
    REQUIRE(Q::DecodeKeys(j));
    j = BundleDocument(); j["bosses"][0]["chain"] = Json::array(); j["bosses"][0]["bools"] = Json::array();
    REQUIRE(Q::DecodeKeys(j));
    j["bosses"][0]["challenge"] = "0xc9"; j["bosses"][0]["trigger"] = "0x37";
    j["bosses"][0]["apply"] = "0x21"; j["activity"]["boss_bool"] = "0x2bc";
    REQUIRE(Q::DecodeKeys(j));
}

TEST_CASE("questdefer decoder rejects unknown fields at every new object", "[oot_coop]") {
    for (auto pointer : {"/unknown", "/activity/unknown", "/bosses/0/unknown", "/bosses/0/chain/0/unknown"}) {
        auto j = BundleDocument(); j[Json::json_pointer(pointer)] = 1;
        CAPTURE(pointer); REQUIRE_FALSE(Q::DecodeKeys(j));
    }
    for (auto pointer : {"/activity/boss_bool", "/activity/death_commands", "/bosses/0/challenge",
         "/bosses/0/enabled", "/bosses/0/name", "/bosses/0/trigger", "/bosses/0/apply",
         "/bosses/0/chain", "/bosses/0/bools", "/bosses/0/region", "/bosses/0/chain/0/challenge",
         "/bosses/0/chain/0/step"}) {
        auto j = BundleDocument(); const Json::json_pointer p{pointer};
        const auto parent = p.parent_pointer(); j[parent].erase(p.back());
        CAPTURE(pointer); REQUIRE_FALSE(Q::DecodeKeys(j));
    }
}

TEST_CASE("questdefer decoder rejects wrong types limits duplicates and ambiguous policies", "[oot_coop]") {
    for (auto field : {"defer_in_activity", "death_skips", "skip_in_place"}) {
        for (Json bad : {Json(0), Json("true"), Json(nullptr)}) {
            auto j = BundleDocument(); j[field] = bad; REQUIRE_FALSE(Q::DecodeKeys(j));
        }
    }
    for (Json bad : {Json("later"), Json(true), Json(1), Json(nullptr)}) {
        auto j = BundleDocument(); j["death_apply"] = bad; REQUIRE_FALSE(Q::DecodeKeys(j));
    }
    for (auto pointer : {"/activity/boss_bool", "/bosses/0/challenge", "/bosses/0/trigger", "/bosses/0/apply",
         "/bosses/0/chain/0/challenge", "/bosses/0/chain/0/step", "/bosses/0/bools/0"}) {
        for (Json bad : {Json(-1), Json(0), Json(true), Json(1.5), Json("0x"), Json("0x100000000"), Json(u64{0x100000000})}) {
            auto j = BundleDocument(); j[Json::json_pointer(pointer)] = bad;
            CAPTURE(pointer, bad); REQUIRE_FALSE(Q::DecodeKeys(j));
        }
    }
    for (auto field : {"death_load_frames", "exit_settle_frames"}) {
        for (Json bad : {Json(0), Json(36001), Json(-1), Json(true), Json(1.5)}) {
            auto j = BundleDocument(); j["activity"][field] = bad; REQUIRE_FALSE(Q::DecodeKeys(j));
        }
        for (int good : {1, 36000}) {
            auto j = BundleDocument(); j["activity"][field] = good; REQUIRE(Q::DecodeKeys(j));
        }
    }
    for (const auto& [pointer, value] : std::vector<std::pair<std::string, Json>>{
        {"/activity", nullptr}, {"/bosses", Json::object()}, {"/bosses/0", false},
        {"/bosses/0/enabled", 1}, {"/bosses/0/name", 1}, {"/bosses/0/name", std::string(33, 'a')},
        {"/bosses/0/name", ""}, {"/bosses/0/chain", false}, {"/bosses/0/bools", false},
        {"/bosses/0/region", Json::array()}, {"/bosses/0/region", {true}},
        {"/activity/death_commands", Json::array()}, {"/activity/death_commands", {1}}}) {
        auto j = BundleDocument(); j[Json::json_pointer(pointer)] = value; CAPTURE(pointer, value);
        REQUIRE_FALSE(Q::DecodeKeys(j));
    }
    for (const auto& [pointer, max] : std::vector<std::pair<std::string, size_t>>{
        {"/activity/death_commands", 16}, {"/bosses/0/region", 8}, {"/bosses/0/bools", 8}, {"/bosses/0/chain", 4}}) {
        auto j = BundleDocument(); auto& array = j[Json::json_pointer(pointer)]; array = Json::array();
        for (size_t n = 0; n <= max; ++n) {
            if (pointer.ends_with("chain")) array.push_back({{"challenge", 300 + n}, {"step", 44}});
            else if (pointer.ends_with("bools")) array.push_back(800 + n);
            else array.push_back("Synthetic_" + std::to_string(n));
            if (n + 1 == max) REQUIRE(Q::DecodeKeys(j));
        }
        REQUIRE_FALSE(Q::DecodeKeys(j));
    }
    auto j = BundleDocument(); const auto boss = j["bosses"][0];
    j["bosses"].push_back(boss); REQUIRE_FALSE(Q::DecodeKeys(j));
    j = BundleDocument(); j.erase("activity"); REQUIRE_FALSE(Q::DecodeKeys(j));
    j = BundleDocument(); j["mirror"]["0xc9"] = {22, 55, 33}; REQUIRE_FALSE(Q::DecodeKeys(j));
    j = BundleDocument(); j["bosses"] = Json::array();
    for (u32 n = 0; n < 17; ++n) {
        auto next = boss; next["challenge"] = 400 + n; j["bosses"].push_back(next);
        if (n == 15) REQUIRE(Q::DecodeKeys(j));
    }
    REQUIRE_FALSE(Q::DecodeKeys(j));
}

TEST_CASE("questdefer defers once and requires sixty consecutive settled exit frames", "[oot_coop]") {
    Rig r; REQUIRE_FALSE(r.Tick()); REQUIRE(r.machine.Status(201) == D::State::Deferred);
    REQUIRE(r.toasts == std::vector<std::string>{"Peer A beat Guardian A. Teleport/die to skip"});
    REQUIRE(r.machine.ExemptBancs() == std::vector<std::string>{"Arena_A"});
    r.Frames(10); REQUIRE(r.toasts.size() == 1);
    r.observation.banc = "Outside_A"; r.Frames(59); REQUIRE(r.calls.empty());
    r.observation.banc = "Arena_A"; r.Tick();
    r.observation.banc = "Outside_A"; r.Frames(59); REQUIRE(r.calls.empty());
    r.observation.loading = true; r.Frames(2); REQUIRE(r.calls.empty());
    r.observation.loading = false; r.Frames(59); REQUIRE(r.calls.empty());
    REQUIRE(r.Tick()); REQUIRE(r.calls.size() == 1); REQUIRE(r.calls[0].step == 33);
    REQUIRE(r.machine.Status(201) == D::State::Apply); REQUIRE(r.machine.ExemptBancs().empty());
}

TEST_CASE("questdefer unknown activity observations fail closed", "[oot_coop]") {
    for (int unknown = 0; unknown < 6; ++unknown) {
        Rig r; r.observation.banc = "Outside_A";
        if (unknown == 0) r.observation.banc.reset();
        if (unknown == 1) r.observation.event.reset();
        if (unknown == 2) r.observation.boss.reset();
        if (unknown == 3) r.observation.banc = "Arena_A";
        if (unknown == 4) r.observation.event = true;
        if (unknown == 5) r.observation.boss = true;
        r.Frames(65); REQUIRE(r.calls.empty()); REQUIRE(r.machine.Status(201) == D::State::Deferred);
    }
}

TEST_CASE("questdefer late snapshot and explicit in place policy use the same machine", "[oot_coop]") {
    for (int mode = 0; mode < 3; ++mode) {
        auto j = BundleDocument();
        if (mode == 1) j["skip_in_place"] = true;
        if (mode == 2) j["defer_in_activity"] = false;
        Rig r{j}; if (mode == 0) r.observation.banc = "Outside_A";
        r.observation.save_loaded = false; r.Frames(3); REQUIRE(r.calls.empty());
        r.observation.save_loaded = true; REQUIRE(r.Tick());
        REQUIRE(r.calls[0] == Q::Target{201, 33, 5, "peer-a"});
    }
    Rig before; before.machine = D::Machine{before.keys, {}};
    before.machine.Queue({201, 44, 2, "peer-a"}); before.Frames(3);
    REQUIRE(before.machine.Status(201) == D::State::Idle); REQUIRE(before.calls.empty());
    before.machine.Queue({201, 33, 5, "peer-a"}); before.Tick();
    REQUIRE(before.machine.Status(201) == D::State::Deferred);
}

TEST_CASE("questdefer teleport applies regardless of death skip policy", "[oot_coop]") {
    for (bool death_skips : {false, true}) {
        auto j = BundleDocument(); j["death_skips"] = death_skips;
        Rig r{j}; r.Tick(); r.machine.Arrival(); REQUIRE(r.Tick()); REQUIRE(r.calls.size() == 1);
    }
}

TEST_CASE("questdefer committed death waits for stable loaded respawn", "[oot_coop]") {
    Rig r; r.Tick(); r.observation.command = "Down_A"; r.Tick();
    r.observation.loading = true; r.observation.stable = false; r.observation.save_loaded = false;
    r.Tick(); REQUIRE(r.machine.Status(201) == D::State::Latched); REQUIRE(r.calls.empty());
    r.Frames(3); REQUIRE(r.calls.empty());
    r.observation.loading = false; r.observation.command = "Live_A";
    r.Tick(); REQUIRE(r.calls.empty());
    r.observation.stable = true; r.observation.save_loaded = true;
    REQUIRE(r.Tick()); REQUIRE(r.Logged("committed death")); REQUIRE(r.calls.size() == 1);
}

TEST_CASE("questdefer revive expired death and disabled death skip never latch", "[oot_coop]") {
    for (int mode = 0; mode < 3; ++mode) {
        auto j = BundleDocument(); if (mode == 2) j["death_skips"] = false;
        Rig r{j}; r.Tick(); r.observation.command = "Down_A"; r.Tick();
        if (mode == 0) { r.observation.command = "Live_A"; r.Tick(); REQUIRE(r.Logged("revive")); }
        if (mode == 1) { r.observation.command.reset(); r.Frames(901); }
        r.observation.loading = true; r.Tick();
        REQUIRE(r.machine.Status(201) == D::State::Deferred); REQUIRE(r.calls.empty());
        r.observation.loading = false; r.observation.command = "Live_A"; r.Tick();
        REQUIRE(r.calls.empty());
        if (mode == 2) REQUIRE(r.Logged("death_skips off"));
    }
}

TEST_CASE("questdefer death during gameover enters Apply but preserves hard loading guard", "[oot_coop]") {
    auto j = BundleDocument(); j["death_apply"] = "during_gameover";
    Rig r{j}; r.Tick(); r.observation.command = "Down_A"; r.Tick();
    r.observation.loading = true; r.Tick();
    REQUIRE(r.machine.Status(201) == D::State::Apply); REQUIRE(r.calls.empty());
    r.observation.loading = false; REQUIRE(r.Tick());
}

TEST_CASE("questdefer local reward chain takes precedence and reports completion once", "[oot_coop]") {
    Rig r; r.Tick(); r.records[201] = Record(201, 3); r.Frames(5);
    REQUIRE(std::count(r.toasts.begin(), r.toasts.end(), "you beat it too") == 1);
    REQUIRE(r.calls.empty()); r.machine.Arrival(); r.Frames(3); REQUIRE(r.calls.empty());
    r.records[201] = Record(201, 5); r.Tick();
    REQUIRE(r.machine.Status(201) == D::State::Done); REQUIRE(r.Logged("not applied: completed locally"));
    Rig completed; completed.records[201] = Record(201, 5); completed.Tick();
    REQUIRE(completed.machine.Status(201) == D::State::Done); REQUIRE(completed.calls.empty());
}

TEST_CASE("questdefer bundle verifies step chain and each Bool in order", "[oot_coop]") {
    Rig r; r.observation.banc = "Outside_A"; REQUIRE(r.Tick());
    REQUIRE(r.calls.size() == 1); REQUIRE(r.bundle_calls == std::vector<bool>{true});
    r.Frames(2); REQUIRE(r.calls.size() == 1); REQUIRE(r.grants.empty());
    r.Commit(); r.Tick(); REQUIRE(r.Logged("element=0 applied"));
    r.records[202] = Record(202, 0); r.Frames(2); REQUIRE(r.calls.size() == 1);
    r.records[202] = Record(202, 1); REQUIRE(r.Tick());
    REQUIRE(r.calls.back() == Q::Target{202, 44, 2, "peer-a"}); REQUIRE_FALSE(r.bundle_calls.back());
    r.Commit(); r.Tick(); REQUIRE(r.Logged("element=1 applied")); REQUIRE(r.grants.empty());
    REQUIRE(r.Tick()); REQUIRE(r.grants == std::vector<u32>{701});
    r.machine.Returned(); r.Tick(); REQUIRE(r.grants.size() == 1);
    r.bools[701] = true; r.Tick(); REQUIRE(r.Logged("element=2 applied"));
    REQUIRE(r.Tick()); REQUIRE(r.grants == std::vector<u32>{701, 702});
    r.bools[702] = true; r.machine.Returned(); r.Tick(); r.Tick();
    REQUIRE(r.machine.Status(201) == D::State::Done);
    REQUIRE(r.toasts.back() == "Guardian A skipped: synced from Peer A");
    REQUIRE(r.machine.Echo(r.records[202])); REQUIRE(r.machine.PrivateBool(701)); REQUIRE_FALSE(r.machine.PrivateBool(999));
}

TEST_CASE("questdefer main step retries three times then gives up for session", "[oot_coop]") {
    Rig r; r.observation.banc = "Outside_A";
    for (size_t attempt = 1; attempt <= 3; ++attempt) {
        REQUIRE(r.Tick()); REQUIRE(r.calls.size() == attempt); r.machine.Returned(); r.Frames(3);
    }
    REQUIRE(r.machine.Status(201) == D::State::Failed); REQUIRE(r.Logged("failed(verify timeout)"));
    r.machine.Epoch(); r.Frames(10); REQUIRE(r.calls.size() == 3); REQUIRE(r.grants.empty());
}

TEST_CASE("questdefer failed chain does not prevent remaining Bool grants", "[oot_coop]") {
    for (bool unavailable : {false, true}) {
        Rig r; r.observation.banc = "Outside_A"; r.Tick(); r.Commit(); r.Tick();
        if (unavailable) { r.records[202] = Record(202, 0); r.Frames(3); }
        else for (int attempt = 0; attempt < 3; ++attempt) {
            REQUIRE(r.Tick()); r.machine.Returned(); r.Frames(3);
        }
        REQUIRE(r.Logged("element=1 failed("));
        REQUIRE(r.Tick()); REQUIRE(r.grants == std::vector<u32>{701});
    }
}

TEST_CASE("questdefer already reached elements have receipts and are never rewritten", "[oot_coop]") {
    Rig r; r.records[202] = Record(202, 4); r.bools[701] = true; r.bools[702] = true;
    r.observation.banc = "Outside_A"; r.Tick(); r.Commit(); r.Frames(5);
    REQUIRE(r.machine.Status(201) == D::State::Done); REQUIRE(r.calls.size() == 1); REQUIRE(r.grants.empty());
    REQUIRE(r.Logged("element=1 already")); REQUIRE(r.Logged("element=2 already")); REQUIRE(r.Logged("element=3 already"));
}

TEST_CASE("questdefer epoch reevaluates current save rather than replaying a completed bundle", "[oot_coop]") {
    Rig r; r.Tick(); r.machine.Epoch(); r.records[201] = Record(201, 5); r.Tick();
    REQUIRE(r.machine.Status(201) == D::State::Done); REQUIRE(r.calls.empty());
    r.machine.Epoch(); r.records[201] = Record(201, 1); r.Tick();
    REQUIRE(r.machine.Status(201) == D::State::Deferred); REQUIRE(r.calls.empty());
}

TEST_CASE("questdefer enabled bosses forward raw steps without mirror floors", "[oot_coop]") {
    auto k = Keys(); Q::Book enabled{k}; REQUIRE(enabled.Mirrored(201)); REQUIRE(enabled.Mirrored(101));
    Q::Book denied{k, {201}}; REQUIRE_FALSE(denied.Mirrored(201));
    auto j = BundleDocument(); j["bosses"][0]["enabled"] = false;
    Q::Book disabled{Keys(j)}; REQUIRE_FALSE(disabled.Mirrored(201));
    REQUIRE_FALSE(enabled.Mirrored(202));
    enabled.Remote(Record(201), {201, 55, 3, "peer-a"}); REQUIRE_FALSE(enabled.Pending(201));
    Rig ignored{j}; ignored.Frames(2); REQUIRE(ignored.calls.empty()); REQUIRE(ignored.machine.Status(201) == D::State::Idle);
    Rig blocked{BundleDocument(), {201}}; blocked.Frames(2); REQUIRE(blocked.calls.empty());
}

TEST_CASE("questdefer ApplyBlock exemption is exact authorized bundle target only", "[oot_coop]") {
    Rig r; auto record = Record(201); record.steps[1].boss = true;
    Q::Manager manager{123, {}, true, false, false, false};
    const Q::Target apply{201, 33, 5, "peer-a"};
    r.Tick(); REQUIRE(Q::ApplyBlock(*r.keys, manager, record, apply, &r.machine) == "minigame or boss step");
    r.machine.Arrival(); r.Tick(false);
    REQUIRE(Q::ApplyBlock(*r.keys, manager, record, apply, &r.machine).empty());
    REQUIRE(Q::ApplyBlock(*r.keys, manager, record, {201, 55, 3, "peer-a"}, &r.machine) == "minigame or boss step");
    REQUIRE(Q::ApplyBlock(*r.keys, manager, record, {201, 66, 4, "peer-a"}, &r.machine) == "minigame or boss step");
    REQUIRE(Q::ApplyBlock(*r.keys, manager, record, apply) == "minigame or boss step");
    manager.restoring = true; REQUIRE_FALSE(Q::ApplyBlock(*r.keys, manager, record, apply, &r.machine).empty());
    manager.restoring = false; record.pending = 44;
    REQUIRE(Q::ApplyBlock(*r.keys, manager, record, apply, &r.machine) == "own request pending");
    record.pending = 0; record.steps[5].participants = 1;
    REQUIRE(Q::ApplyBlock(*r.keys, manager, record, apply, &r.machine) == "participants");
    record.steps[5].participants = 0;
    auto j = BundleDocument(); j["bosses"][0]["enabled"] = false; Rig disabled{j};
    disabled.machine.Arrival(); disabled.Tick();
    REQUIRE(Q::ApplyBlock(*disabled.keys, manager, record, apply, &disabled.machine) == "minigame or boss step");
    j = BundleDocument(); j["skip_in_place"] = true; Rig skip{j}; skip.Tick(false);
    REQUIRE(Q::ApplyBlock(*skip.keys, manager, record, apply, &skip.machine).empty());
}

TEST_CASE("questdefer reentry revokes an unissued outside-region apply authorization", "[oot_coop]") {
    Rig r; r.observation.banc = "Outside_A"; r.Tick(false);
    const Q::Target target{201, 33, 5, "peer-a"};
    REQUIRE(r.machine.Authorizes(target)); REQUIRE(r.calls.empty());
    r.observation.banc = "Arena_A"; r.Tick(false);
    REQUIRE_FALSE(r.machine.Authorizes(target));
    auto record = Record(201); record.steps[1].boss = true;
    Q::Manager manager{123, {}, true, false, false, false};
    REQUIRE(Q::ApplyBlock(*r.keys, manager, record, target, &r.machine) == "minigame or boss step");
}

TEST_CASE("questdefer no issuing until all observation and applier gates open", "[oot_coop]") {
    for (int guard = 0; guard < 6; ++guard) {
        Rig r; r.observation.banc = "Outside_A";
        if (guard == 0) r.observation.stable = false;
        if (guard == 1) r.observation.save_loaded = false;
        if (guard == 2) r.observation.loading = true;
        if (guard == 3) r.observation.loading.reset();
        if (guard == 4) r.observation.event.reset();
        r.Frames(3, guard != 5); REQUIRE(r.calls.empty()); REQUIRE(r.grants.empty());
    }
}

TEST_CASE("questdefer local completion supersedes unissued Apply intent", "[oot_coop]") {
    for (u32 local_index : {3U, 5U}) {
        Rig r; r.observation.banc = "Outside_A"; r.Tick(false);
        REQUIRE(r.machine.Status(201) == D::State::Apply); REQUIRE(r.calls.empty());
        r.records[201] = Record(201, local_index); r.Tick(); r.Frames(4);
        REQUIRE(r.calls.empty()); REQUIRE(r.grants.empty());
        if (local_index == 3) {
            REQUIRE(std::count(r.toasts.begin(), r.toasts.end(), "you beat it too") == 1);
            REQUIRE(r.machine.Status(201) != D::State::Done);
            r.records[201] = Record(201, 5); r.Tick();
        }
        REQUIRE(r.machine.Status(201) == D::State::Done);
        REQUIRE(r.Logged("not applied: completed locally"));
    }
}

TEST_CASE("questdefer death held on the game over menu latches and waits for a live respawn", "[oot_coop]") {
    // Field C17 sB: Dead, then the Game Over menu waited 28 s (1703 frames > death_load_frames)
    // before the load rose with the command still held; after the load the player stayed in the
    // death command for 7 s while the scene restored.
    Rig r; r.Tick(); r.observation.command = "Down_A"; r.Tick();
    r.Frames(1700); REQUIRE(r.machine.Status(201) == D::State::Deferred);
    r.observation.loading = true; r.Tick();
    REQUIRE(r.machine.Status(201) == D::State::Latched); REQUIRE(r.Logged("committed death: latched"));
    r.observation.loading = false; r.Frames(30);
    REQUIRE(r.calls.empty()); REQUIRE(r.machine.Status(201) == D::State::Latched);
    r.observation.command = "Live_A"; REQUIRE(r.Tick());
    REQUIRE(r.Logged("respawned: live command Live_A")); REQUIRE(r.calls.size() == 1);
}

TEST_CASE("questdefer completed epoch clears consumed teleport and death permissions", "[oot_coop]") {
    for (bool death : {false, true}) {
        Rig r; r.Tick();
        if (death) {
            r.observation.command = "Down_A"; r.Tick(); r.observation.loading = true; r.Tick();
            r.observation.loading = false; r.observation.command = "Live_A";
        } else r.machine.Arrival();
        REQUIRE(r.Tick()); r.Commit(); r.Tick();
        REQUIRE(r.Tick()); r.Commit(); r.Tick();
        REQUIRE(r.Tick()); r.bools[701] = true; r.machine.Returned(); r.Tick();
        REQUIRE(r.Tick()); r.bools[702] = true; r.machine.Returned(); r.Tick(); r.Tick();
        REQUIRE(r.machine.Status(201) == D::State::Done);
        const auto calls = r.calls.size(), grants = r.grants.size();
        r.machine.Epoch(); r.records[201] = Record(201); r.Tick(); r.Frames(4);
        REQUIRE(r.machine.Status(201) == D::State::Deferred);
        REQUIRE(r.calls.size() == calls); REQUIRE(r.grants.size() == grants);
        REQUIRE_FALSE(r.machine.Authorizes({201, 33, 5, "peer-a"}));
    }
}

TEST_CASE("questdefer runs when the session samples only watched machines", "[oot_coop]") {
    // Review check: the session takes an Observation only while Watching(); a queued team target
    // must make the machine watched by itself (no observation is taken for an idle machine).
    Rig rig;
    rig.observation.banc = std::string{"Field_A"};
    REQUIRE(rig.machine.Watching());
    for (int i = 0; i < 3; ++i) {
        ++rig.frame;
        if (rig.machine.Watching()) rig.machine.Observe(rig.observation, rig.frame, rig.port);
        rig.machine.Frame(rig.port, rig.frame, true);
    }
    REQUIRE(rig.machine.Watching());
    REQUIRE_FALSE(rig.calls.empty());
    REQUIRE(rig.calls.front().challenge == 201);
    REQUIRE(rig.calls.front().step == 33);
    REQUIRE(rig.Logged("watch frame="));
}

TEST_CASE("questdefer outside applies over a stale restoration NotReady pending (field C17 F-A)", "[oot_coop]") {
    // Field C17 F-A: fresh New Game, the local quest never started (Ready), the restoration burst
    // left pending=NotReady with no request flag; a teammate's kill arrives while outside the region.
    const Q::Target apply{201, 33, 5, "peer-a"};
    const Q::Manager manager{123, {}, true, false, false, false};
    Rig r; r.records[201].pending = 11; r.observation.banc = "Outside_A";
    REQUIRE(r.Tick()); REQUIRE(r.calls == std::vector<Q::Target>{apply});
    REQUIRE(r.bundle_calls == std::vector<bool>{true}); REQUIRE_FALSE(r.Logged("waiting: own request pending"));
    // The issue path's last guard admits only the authorized bundle target; the mirror path keeps the old rule.
    REQUIRE(Q::ApplyBlock(*r.keys, manager, r.records[201], apply, &r.machine).empty());
    REQUIRE(Q::ApplyBlock(*r.keys, manager, r.records[201], apply) == "own request pending");
    r.Commit(); r.Tick(); REQUIRE(r.Logged("element=0 applied"));
    REQUIRE(r.Tick()); r.Commit(); r.Tick(); REQUIRE(r.Logged("element=1 applied"));
    REQUIRE(r.Tick()); r.bools[701] = true; r.machine.Returned(); r.Tick();
    REQUIRE(r.Tick()); r.bools[702] = true; r.machine.Returned(); r.Tick(); r.Tick();
    REQUIRE(r.machine.Status(201) == D::State::Done);
    REQUIRE(r.toasts.back() == "Guardian A skipped: synced from Peer A");

    // Still an own request: the request flag is set, or the pending hash is not NotReady.
    for (int mode = 0; mode < 2; ++mode) {
        Rig held; held.observation.banc = "Outside_A";
        if (mode == 0) { held.records[201].pending = 11; held.records[201].flags = 4; }
        else held.records[201].pending = 44;
        held.Frames(5); REQUIRE(held.calls.empty()); REQUIRE(held.machine.Status(201) == D::State::Apply);
        REQUIRE(std::count_if(held.logs.begin(), held.logs.end(), [](const auto& s) {
            return s.find("waiting: own request pending") != std::string::npos; }) == 1);
        REQUIRE(Q::ApplyBlock(*held.keys, manager, held.records[201], apply, &held.machine) == "own request pending");
    }

    // Inside the region the same record still defers once; a teleport arrival then applies.
    Rig inside; inside.records[201].pending = 11;
    inside.Frames(3); REQUIRE(inside.machine.Status(201) == D::State::Deferred); REQUIRE(inside.calls.empty());
    REQUIRE(inside.toasts == std::vector<std::string>{"Peer A beat Guardian A. Teleport/die to skip"});
    inside.machine.Arrival(); REQUIRE(inside.Tick()); REQUIRE(inside.calls == std::vector<Q::Target>{apply});
}
