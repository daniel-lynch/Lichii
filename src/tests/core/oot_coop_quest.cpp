// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstring>
#include <catch2/catch_test_macros.hpp>
#include "core/arm/dynarmic/oot_coop_quest.h"
#include "core/arm/dynarmic/oot_coop_sync.h"
#include "core/arm/dynarmic/oot_coop_apply.h"
#include "tests/core/oot_test_profile.h"

namespace {
namespace Q = Core::OotCoop::Quest;
namespace C = Core::OotCoop;
namespace T = Core::OotGameDataTrace;
using Json = nlohmann::json;
// Entirely synthetic layout and policy. The two instructions are architectural hints.
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
std::shared_ptr<Q::Keys> Keys() {
    const auto k = Q::DecodeKeys(Document());
    REQUIRE(k);
    auto result = std::make_shared<Q::Keys>(*k);
    result->settle_frames = 2;
    result->apply_timeout = 3;
    return result;
}
Q::Record Record(u32 index = 1) {
    Q::Record r{101, index, 0, 0, 0, {{11}, {22}, {44}, {55}, {66}, {33}}};
    r.current = r.steps[index].hash;
    return r;
}
struct Image {
    static constexpr u64 Base = 0x1000, Manager = 0x2000, RecordAt = 0x3000, Steps = 0x6000;
    std::vector<u8> bytes = std::vector<u8>(0x10000);
    std::shared_ptr<Q::Keys> keys = Keys();
    std::vector<Q::Target> sent;
    std::vector<std::string> logs;
    std::vector<std::array<u64, 8>> calls;
    bool ready{true}, send_ok{true};
    Q::Port port;
    template <typename V> void Put(u64 at, V v) { std::memcpy(bytes.data() + at, &v, sizeof(v)); }
    template <typename V> std::optional<V> Get(u64 at) const {
        if (at >= bytes.size() || sizeof(V) > bytes.size() - at) return {};
        V value{}; std::memcpy(&value, bytes.data() + at, sizeof(value)); return value;
    }
    Image() {
        port.range = [&](u64 at, u64 size) { return at && at < bytes.size() && size <= bytes.size() - at; };
        port.r8 = [&](u64 at) { return Get<u8>(at); };
        port.r16 = [&](u64 at) { return Get<u16>(at); };
        port.r32 = [&](u64 at) { return Get<u32>(at); };
        port.r64 = [&](u64 at) { return Get<u64>(at); };
        port.ready = [&] { return ready; };
        port.call = [&](const Q::Entry&, const std::array<u64, 8>& args) { calls.push_back(args); return true; };
        port.send = [&](const Q::Target& t) { if (!send_ok) return false; sent.push_back(t); return true; };
        port.log = [&](std::string_view s) { logs.emplace_back(s); };
        Put<u64>(Base + keys->M("cell"), Manager);
        Put<u64>(Base + keys->M("got_cell"), Base + keys->M("cell"));
        Put<u8>(Manager + keys->M("initialized"), 1);
        for (size_t i = 0; i < 3; ++i) {
            Put<u32>(Manager + keys->tables[i].count, 1);
            Put<u64>(Manager + keys->tables[i].data, RecordAt + i * 0x800);
            const u64 at = RecordAt + i * 0x800, state = at + keys->L("record.state_embedded");
            Put<u32>(at + keys->L("record.challenge"), static_cast<u32>(101 + i));
            Put<u64>(state + keys->L("state.record_backlink"), at);
            Put<u32>(state + keys->L("state.step_count"), 6);
            Put<u64>(state + keys->L("state.step_array"), Steps);
            Put<u32>(state + keys->L("state.current_index"), 1);
            Put<u32>(at + keys->L("record.current_hash"), 22);
        }
        const auto r = Record();
        for (size_t i = 0; i < r.steps.size(); ++i) Put<u32>(Steps + i * keys->L("step.stride") + keys->L("step.hash"), r.steps[i].hash);
    }
    void Commit(u32 index) {
        Put<u32>(RecordAt + keys->L("record.state_embedded") + keys->L("state.current_index"), index);
        Put<u32>(RecordAt + keys->L("record.current_hash"), Record(index).current);
    }
    bool Has(std::string_view text) const {
        return std::any_of(logs.begin(), logs.end(), [&](const auto& s) { return s.find(text) != std::string::npos; });
    }
    void Settle(Q::Controller& c) { for (u64 f = 100; f <= 102; ++f) c.Frame(port, f, 0, true, true, true); }
};
}
TEST_CASE("Quest private schema is strict and defaults disabled", "[oot_coop]") {
    const auto j = Document();
    const auto k = Q::DecodeKeys(j);
    REQUIRE(k);
    REQUIRE_FALSE(k->enabled);
    REQUIRE(k->settle_frames == 300);
    REQUIRE(k->apply_timeout == 600);
    for (const auto& pointer : {"/warning", "/manager/other", "/layout/record/other", "/entries/other", "/setter_arguments/mode"}) {
        auto bad = j; bad[Json::json_pointer(pointer)] = 1;
        REQUIRE_FALSE(Q::DecodeKeys(bad));
    }
    for (const auto& [pointer, value] : std::vector<std::pair<std::string, Json>>{
        {"/enabled", 1}, {"/manager/cell", 3}, {"/manager/got_dereferences", 1},
        {"/manager/record_stride", 16}, {"/setter_arguments/w5", 1}, {"/setter_arguments/w4", 1},
        {"/setter_arguments/w3", 0}, {"/layout/step/hash", 1000}, {"/apply_timeout", 0},
        {"/settle_frames", -1}, {"/mirror/0x65", Json::array({22, 11})},
        {"/commit_observation/0/words/0", 0}, {"/commit_observation/0/record_from_state", "-0x20"},
        {"/commit_observation/0/state_register", 31}, {"/notready_hash", true}}) {
        auto bad = j; bad[Json::json_pointer(pointer)] = value;
        REQUIRE_FALSE(Q::DecodeKeys(bad));
    }
    auto enabled = j; enabled["enabled"] = true;
    auto document = Json{{"offsets", {{OotTestProfile::BuildId, {{"quest", enabled}}}}}};
    const auto profile = C::DecodeProfile(document, OotTestProfile::BuildId);
    REQUIRE(profile);
    REQUIRE(profile->quest->enabled);
    REQUIRE(profile->SiteOffset(T::Point::StepCommit) == 0x240);
    REQUIRE_FALSE(T::CoopPoint(T::Point::StepCommit));
    REQUIRE(T::InstructionFor(*profile, 0x1000, 0x1240, T::Pacibsp) == T::Pacibsp);
    const auto extras = C::DecodeKeyExtras(Json{{"deny", {{"quest", Json::array({"0x65"})}}}});
    REQUIRE(extras);
    REQUIRE(extras->deny_quest.contains(101));
    REQUIRE(extras->deny.empty());
}
TEST_CASE("Quest Book floors raw targets and preserves the furthest", "[oot_coop]") {
    auto k = Keys(); Q::Book b{k}; auto r = Record(); b.Snapshot(r);
    REQUIRE(b.Remote(r, {101, 55, 3, "peer"}) == "queued");
    REQUIRE(b.Pending(101)->index == 2);
    REQUIRE(b.Pending(101)->step == 44);
    REQUIRE(b.Remote(r, {101, 66, 4, "peer"}) == "queued");
    REQUIRE(b.Pending(101)->index == 4);
    REQUIRE(b.Remote(r, {101, 44, 2, "peer"}) == "equal or backwards team target");
    REQUIRE(b.Pending(101)->index == 4);
    REQUIRE(b.Remote(r, {101, 55, 5, "peer"}) == "unknown step or index mismatch");
    REQUIRE(b.Remote(r, {101, 99, 5, "peer"}) == "unknown step or index mismatch");
    REQUIRE(b.Remote(r, {999, 66, 4, "peer"}) == "not mirrored or denied");
    Q::Book denied{k, {101}};
    REQUIRE(denied.Remote(r, {101, 66, 4, "peer"}) == "not mirrored or denied");
    b.Epoch();
    REQUIRE_FALSE(b.Known(101));
    REQUIRE(b.Reconcile(r) == "queued");
    REQUIRE(b.Pending(101)->index == 4);
    b.Issued(*b.Pending(101));
    REQUIRE(b.Echo(Record(4)));
    REQUIRE_FALSE(b.Echo(Record(3)));
    b.Applied(*b.Pending(101));
    REQUIRE(b.Reconcile(Record(4)) == "already at or past safe floor");
}
TEST_CASE("Quest NotReady waits Complete drops and attempts are bounded", "[oot_coop]") {
    auto k = Keys(); Q::Book b{k}; auto r = Record(0);
    REQUIRE(b.Remote(r, {101, 66, 4, "peer"}) == "NotReady: waiting");
    Q::Manager manager{123, {}, true, false, false, false};
    REQUIRE_FALSE(Q::CanApply(*k, manager, r, *b.Pending(101)));
    r = Record(); REQUIRE(b.Reconcile(r) == "queued");
    REQUIRE(Q::CanApply(*k, manager, r, *b.Pending(101)));
    const auto target = *b.Pending(101);
    for (int attempt = 0; attempt < 3; ++attempt) {
        b.Issued(target); REQUIRE(b.Failed(101) == (attempt == 2));
    }
    b.Epoch(); REQUIRE(b.Reconcile(r) == "gave up for session");
    Q::Book complete{k};
    REQUIRE(complete.Remote(Record(5), {101, 33, 5, "peer"}) == "Complete: dropped");
    REQUIRE_FALSE(complete.Pending(101));
    const auto baseline = complete.Baseline({Record(0), Record(1), Record(3)});
    REQUIRE(baseline.size() == 1);
    REQUIRE(baseline[0].step == 55); // raw committed transient, never relabelled safe
    REQUIRE(baseline[0].index == 3);
}
TEST_CASE("Quest reader checks all tables bounds backlink and commit agreement", "[oot_coop]") {
    Image im;
    const auto m = Q::ReadManager(im.port, *im.keys, im.Base);
    REQUIRE(m); REQUIRE(m->Ready());
    for (u32 c : {101U, 102U, 103U}) REQUIRE(Q::ReadRecord(im.port, *im.keys, *m, c));
    REQUIRE_FALSE(Q::ReadRecord(im.port, *im.keys, *m, 104));
    SECTION("counts bounded") {
        im.Put<u32>(im.Manager + im.keys->tables[2].count, 257);
        REQUIRE_FALSE(Q::ReadManager(im.port, *im.keys, im.Base));
    }
    SECTION("GOT mismatch") {
        im.Put<u64>(im.Base + im.keys->M("got_cell"), 0x6000);
        im.Put<u64>(0x6000, 0x7000);
        bool mismatch{}; REQUIRE_FALSE(Q::ReadManager(im.port, *im.keys, im.Base, &mismatch)); REQUIRE(mismatch);
    }
    SECTION("unreadable table") {
        im.Put<u64>(im.Manager + im.keys->tables[1].data, ~u64{0});
        REQUIRE_FALSE(Q::ReadManager(im.port, *im.keys, im.Base));
    }
    SECTION("invalid backlink") {
        im.Put<u64>(im.RecordAt + im.keys->L("record.state_embedded"), 0);
        REQUIRE_FALSE(Q::ReadRecord(im.port, *im.keys, *m, 101));
    }
    SECTION("index not yet cached") {
        im.Put<u32>(im.RecordAt + im.keys->L("record.current_hash"), 44);
        REQUIRE_FALSE(Q::ReadRecord(im.port, *im.keys, *m, 101));
    }
    SECTION("duplicate hashes") {
        im.Put<u32>(im.Steps + im.keys->L("step.stride"), 11);
        REQUIRE_FALSE(Q::ReadRecord(im.port, *im.keys, *m, 101));
    }
    SECTION("unreadable steps") {
        im.Put<u64>(im.RecordAt + im.keys->L("record.state_embedded") + im.keys->L("state.step_array"), 0xfff0);
        REQUIRE_FALSE(Q::ReadRecord(im.port, *im.keys, *m, 101));
    }
}
TEST_CASE("Quest issue guards check both participant lists and pending requests", "[oot_coop]") {
    auto k = Keys(); auto r = Record(); Q::Target target{101, 66, 4, "peer"};
    Q::Manager m{123, {}, true, false, false, false};
    REQUIRE(Q::CanApply(*k, m, r, target));
    SECTION("old participants") { r.steps[r.index].participants = 1; }
    SECTION("new participants") { r.steps[4].participants = 1; }
    SECTION("minigame") { r.steps[4].minigame = true; }
    SECTION("boss") { r.steps[4].boss = true; }
    SECTION("pending hash") { r.pending = 55; }
    SECTION("pending flag") { r.flags = 4; }
    SECTION("restoration flag") { r.flags = 2; }
    SECTION("busy") { m.busy = true; }
    SECTION("restoring") { m.restoring = true; }
    SECTION("manager suppressed") { m.gated = true; }
    REQUIRE_FALSE(Q::CanApply(*k, m, r, target));
}
TEST_CASE("Quest controller settles polls raw commits and suppresses replay", "[oot_coop]") {
    Image im; Q::Controller c{im.keys, {}, im.Base}; im.Settle(c);
    REQUIRE(im.sent.empty());
    SECTION("poll catches a missed trap") {
        im.Commit(3); c.Frame(im.port, 120, 0, true, true, true);
        REQUIRE(im.sent.size() == 1); REQUIRE(im.sent[0].step == 55); REQUIRE(im.sent[0].index == 3);
        c.Frame(im.port, 180, 0, true, true, true); REQUIRE(im.sent.size() == 1);
    }
    SECTION("remote floors then verifies without echo") {
        c.Queue({101, 55, 3, "peer"});
        REQUIRE(c.Frame(im.port, 103, 0, true, true, true)); REQUIRE(im.calls.size() == 1);
        REQUIRE(im.calls[0] == std::array<u64, 8>{im.Manager, 101, 44, 1, 0, 0, 0, 0});
        c.Returned(im.port, 1); im.Commit(2);
        c.Dirty(im.port, im.RecordAt + im.keys->L("record.state_embedded"), im.Base + 0x240);
        c.Frame(im.port, 104, 0, true, true, true);
        REQUIRE(im.Has("quest applied")); REQUIRE(im.Has("quest echo suppressed")); REQUIRE(im.sent.empty());
    }
    SECTION("trap restoration attribution survives byte clear") {
        im.Put<u8>(im.Manager + im.keys->M("restoring"), 1);
        c.Dirty(im.port, im.RecordAt + im.keys->L("record.state_embedded"), im.Base + 0x240);
        im.Put<u8>(im.Manager + im.keys->M("restoring"), 0); im.Commit(2);
        c.Frame(im.port, 103, 0, true, true, true);
        REQUIRE(im.sent.empty()); REQUIRE(im.Has("restoration"));
    }
    SECTION("busy frame and event defer") {
        c.Queue({101, 66, 4, "peer"}); im.ready = false;
        c.Frame(im.port, 103, 0, true, true, true); REQUIRE(im.calls.empty());
        im.ready = true; im.Put<u8>(im.Manager + im.keys->M("busy"), 1);
        c.Frame(im.port, 104, 0, true, true, true); REQUIRE(im.calls.empty());
        im.Put<u8>(im.Manager + im.keys->M("busy"), 0);
        REQUIRE(c.Frame(im.port, 105, 0, true, true, true));
    }
    SECTION("no capability no packets or calls") {
        c.Queue({101, 66, 4, "peer"}); im.Commit(2);
        c.Frame(im.port, 120, 0, true, false, true);
        REQUIRE(im.calls.empty()); REQUIRE(im.sent.empty());
    }
}
TEST_CASE("Quest epoch revalidates targets and settles before reissue", "[oot_coop]") {
    Image im; Q::Controller c{im.keys, {}, im.Base}; im.Settle(c);
    c.Queue({101, 66, 4, "peer"}); REQUIRE(c.Frame(im.port, 103, 0, true, true, true)); c.Returned(im.port, 1);
    im.Put<u32>(im.Manager + im.keys->tables[2].count, 0);
    REQUIRE_FALSE(c.Frame(im.port, 104, 0, true, true, true));
    REQUIRE_FALSE(c.Frame(im.port, 105, 0, true, true, true));
    REQUIRE(c.Frame(im.port, 106, 0, true, true, true));
    REQUIRE(im.calls.size() == 2);
}
TEST_CASE("Quest wire accepts legacy shape and separate snapshots strictly", "[oot_coop]") {
    C::Change change{C::Kind::Quest, "peer", 1, 101, 3, 55};
    auto packet = C::EncodeChange(change);
    REQUIRE(C::DecodeChange(packet) == change);
    packet["rev"] = 2; REQUIRE(C::DecodeChange(packet) == change);
    packet["other"] = 1; REQUIRE_FALSE(C::DecodeChange(packet)); packet.erase("other"); packet.erase("rev");
    for (const auto& [name, value] : std::vector<std::pair<std::string, Json>>{
        {"challenge", 0}, {"step", 0}, {"index", 256}, {"index", true}, {"challenge", -1}}) {
        auto bad = packet; bad[name] = value; REQUIRE_FALSE(C::DecodeChange(bad));
    }
    packet.erase("origin"); packet.erase("seq");
    REQUIRE(C::DecodeChange(packet, true));
    const auto snapshot = Q::DecodeSnapshot(Json::array({packet}));
    REQUIRE(snapshot); REQUIRE(snapshot->at(0).index == 3);
    REQUIRE_FALSE(Q::DecodeSnapshot(Json::array({packet, packet})));
    packet["rev"] = 1; REQUIRE_FALSE(Q::DecodeSnapshot(Json::array({packet})));
    C::Hello hello; hello.build_id = OotTestProfile::BuildId; hello.client_id = "peer";
    REQUIRE_FALSE(C::EncodeHello(hello).contains("quest"));
    hello.quest = true; REQUIRE(C::EncodeHello(hello).at("quest") == 1);
}
TEST_CASE("Quest timeout retries at most twice and leaves the class idle", "[oot_coop]") {
    Image im; Q::Controller c{im.keys, {}, im.Base}; im.Settle(c);
    c.Queue({101, 66, 4, "peer"});
    REQUIRE(c.Frame(im.port, 103, 0, true, true, true)); c.Returned(im.port, 0);
    REQUIRE(c.Frame(im.port, 106, 0, true, true, true)); c.Returned(im.port, 0);
    REQUIRE(c.Frame(im.port, 109, 0, true, true, true)); c.Returned(im.port, 0);
    REQUIRE_FALSE(c.Frame(im.port, 112, 0, true, true, true));
    REQUIRE_FALSE(c.Frame(im.port, 180, 0, true, true, true));
    REQUIRE(im.calls.size() == 3);
    REQUIRE(im.Has("gave up for session"));
}
TEST_CASE("Quest first stable frame and bulk changes rebaseline silently", "[oot_coop]") {
    Image im; im.keys->mirror[102] = im.keys->mirror[101];
    Q::Controller c{im.keys, {}, im.Base};
    c.Frame(im.port, 100, 0, true, true, true);
    im.Commit(2);
    c.Frame(im.port, 102, 0, true, true, true);
    REQUIRE(im.sent.size() == 1); REQUIRE(im.Has("quest baseline"));
    REQUIRE_FALSE(im.Has(" forwarded (committed)"));
    im.sent.clear();
    im.Commit(4);
    im.Put<u32>(im.RecordAt + 0x800 + im.keys->L("record.current_hash"), 44);
    im.Put<u32>(im.RecordAt + 0x800 + im.keys->L("record.state_embedded") + im.keys->L("state.current_index"), 2);
    c.Frame(im.port, 120, 0, true, true, true);
    REQUIRE(im.sent.empty()); REQUIRE(im.Has("bulk change"));
    c.Frame(im.port, 122, 0, true, true, true);
    REQUIRE(im.sent.size() == 2); // the new epoch's baselines, not gameplay forwards
}
TEST_CASE("Quest logging is bounded across repeated epochs", "[oot_coop]") {
    Image im; Q::Controller c{im.keys, {}, im.Base};
    for (unsigned i = 0; i < 600; ++i) c.Log(im.port, "synthetic");
    REQUIRE(im.logs.size() == 511); // one session line reserved for transport fallback
    c.Frame(im.port, 3600, 0, false, false, false);
    REQUIRE(im.logs.size() == 512); REQUIRE(im.Has("suppressed lines="));
    for (const auto& line : im.logs) REQUIRE(line.starts_with("OoT co-op: quest "));
}

TEST_CASE("Quest can only use its guarded applier", "[oot_coop]") {
    C::ApplyPolicy policy{true, {C::Kind::Quest}, {{C::Kind::Quest, 101}}};
    REQUIRE_FALSE(policy.Allows({C::Kind::Quest, "peer", 1, 101, 1, 22}));
}

TEST_CASE("Quest epoch changes cannot extend the session attempt budget", "[oot_coop]") {
    Q::Book b{Keys()}; const auto r = Record();
    REQUIRE(b.Remote(r, {101, 66, 4, "peer"}) == "queued");
    for (unsigned i = 0; i < 3; ++i) {
        b.Issued(*b.Pending(101));
        b.Epoch();
        REQUIRE(b.Reconcile(r) == (i == 2 ? "gave up for session" : "queued"));
    }
    REQUIRE_FALSE(b.Pending(101));
}
