// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <chrono>
#include <filesystem>
#include <fstream>
#include <catch2/catch_test_macros.hpp>
#include "core/arm/dynarmic/oot_coop_teleport.h"
#include "core/arm/dynarmic/oot_coop_sync.h"

namespace {
namespace T = Core::OotCoop::Teleport;
using Json = nlohmann::json;
Json KeyJson() {
    return {{"enabled", true}, {"actor_pos", "0x100"}, {"event_active", 32}, {"block_bools", {123, 456}},
        {"requester_commands", {"StandA"}}, {"answer_commands", {"StandA", "StandB"}},
        {"allow_bancs", {"Field_A"}}, {"deny_bancs", Json::array()}, {"settle_frames", 3},
        {"settle_epsilon", 0.01}, {"agree_epsilon", 0.05}, {"tolerance", {0.1, 0.2}},
        {"verify_frames", 4}, {"reply_timeout_ms", 5000}, {"apply_timeout_ms", 500}};
}
T::Keys Keys() { return *T::DecodeKeys(KeyJson()); }
T::Sample Sample() {
    T::Sample s;
    s.banc = "Field_A"; s.child = true; s.loading = false;
    s.event_running = 0; s.block_bools_ok = true; s.command = "StandA";
    s.info_pos = {1, 2, 3}; s.actor_pos = s.info_pos; s.yaw = 90;
    s.resident = Core::OotCoop::Player::Resident{0x1000, 0x2000, 0x3000, 7};
    return s;
}
T::Packet Reply(u64 id = 1) {
    T::Packet p;
    p.id = id; p.peer = "peer-a"; p.ok = true; p.child = true;
    p.banc = "Field_A"; p.pos = {4, 5, 6}; p.yaw = -45;
    return p;
}
struct Rig {
    T::Controller controller{Keys()};
    T::Sample sample{Sample()};
    std::vector<std::string> logs;
    std::vector<Core::OotCoop::ApplyTarget> calls;
    bool valid{true}, valid_at_call{true};
    T::Port port{
        [&](const auto& resident) { return valid && sample.resident == resident; },
        [&](const auto& resident, const auto& packet) {
            if (!valid_at_call || sample.resident != resident) return false;
            calls.push_back(T::MatrixCall(0x4000, 0x5000, 0x6000, resident.actor, packet)); return true;
        }, [&](std::string_view s) { logs.emplace_back(s); }};
    u64 now{1000};
    Rig() { for (int i = 0; i < 3; ++i) Frame(); }
    bool Frame(bool can_call = true) { now += 16; return controller.Frame(sample, now, can_call, port); }
    void Request() { REQUIRE(controller.Trigger("peer-a", now, port.log)); controller.TakeOutput(); }
    void Receive(T::Packet p = Reply()) { controller.Receive(p, now, port.log); }
    bool Logged(std::string_view text) const {
        return std::any_of(logs.begin(), logs.end(), [&](const auto& s) { return s.find(text) != std::string::npos; });
    }
};
}

TEST_CASE("teleport keys are strict bounded and optional fall origin", "[oot_coop]") {
    const auto good = KeyJson();
    REQUIRE(T::DecodeKeys(good));
    for (const auto& [name, value] : good.items()) {
        auto j = good; j.erase(name);
        CAPTURE(name);
        REQUIRE_FALSE(T::DecodeKeys(j));
    }
    auto j = good; j["unknown"] = 1; REQUIRE_FALSE(T::DecodeKeys(j));
    for (const auto& key : {"actor_pos", "event_active", "settle_frames", "verify_frames", "reply_timeout_ms", "apply_timeout_ms"}) {
        for (Json bad : {Json(-1), Json(true), Json(1.5), Json("0x"), Json(0x10000001)}) {
            j = good; j[key] = bad; REQUIRE_FALSE(T::DecodeKeys(j));
        }
    }
    for (const auto& key : {"settle_epsilon", "agree_epsilon", "tolerance"}) {
        j = good; j[key] = nullptr; REQUIRE_FALSE(T::DecodeKeys(j));
    }
    for (const auto& key : {"requester_commands", "answer_commands", "allow_bancs", "block_bools"}) {
        j = good; j[key] = Json::array(); REQUIRE_FALSE(T::DecodeKeys(j));
        j[key] = std::vector<int>(129, 1); REQUIRE_FALSE(T::DecodeKeys(j));
    }
    j = good; j["fall_origin"] = {{"chain", {16, 24}}, {"owner", 8}, {"field", 32}, {"bit", 1}};
    REQUIRE(T::DecodeKeys(j)->fall_origin);
    for (const auto& field : {"chain", "owner", "field", "bit"}) {
        auto bad = j; bad["fall_origin"].erase(field); REQUIRE_FALSE(T::DecodeKeys(bad));
    }
    for (Json bit : {Json(0), Json(3), Json(65536), Json(true)}) {
        auto bad = j; bad["fall_origin"]["bit"] = bit; REQUIRE_FALSE(T::DecodeKeys(bad));
    }
    j["fall_origin"]["extra"] = 0; REQUIRE_FALSE(T::DecodeKeys(j));
}

TEST_CASE("teleport packets validate every field and round trip", "[oot_coop]") {
    auto p = Reply();
    for (bool server : {false, true}) {
        auto j = T::EncodePacket(p, server);
        REQUIRE(T::DecodePacket(j, server));
        for (const auto& [name, value] : j.items()) {
            auto bad = j; bad.erase(name); REQUIRE_FALSE(T::DecodePacket(bad, server));
        }
        auto bad = j; bad[server ? "target" : "origin"] = "peer-b";
        REQUIRE_FALSE(T::DecodePacket(bad, server));
    }
    const auto good = T::EncodePacket(p);
    for (const Json& delta : {Json{{"type", "bad"}}, Json{{"target", "bad id"}}, Json{{"target", ""}},
        Json{{"request_id", 0}}, Json{{"request_id", (u64{1} << 53)}}, Json{{"request_id", true}}, Json{{"request_id", 1.0}},
        Json{{"status", "bad"}}, Json{{"banc", ""}}, Json{{"banc", "bad/name"}}, Json{{"room", 256}},
        Json{{"room", -1}}, Json{{"room", false}}, Json{{"pos", {1, 2}}}, Json{{"pos", {true, 2, 3}}},
        Json{{"pos", {100001, 2, 3}}}, Json{{"yaw", 361}}, Json{{"yaw", false}}, Json{{"age", "other"}},
        Json{{"extra", 0}}, Json{{"yaw", std::numeric_limits<double>::infinity()}}}) {
        auto bad = good; bad.update(delta); REQUIRE_FALSE(T::DecodePacket(bad, false));
    }
    p.room = 0; REQUIRE(T::DecodePacket(T::EncodePacket(p), false)->room == 0);
    p.request = true; REQUIRE(T::DecodePacket(T::EncodePacket(p), false)->request);
    p.request = false; p.ok = false;
    for (auto reason : {"loading", "event", "pause", "state", "airborne", "scene", "busy", "unready", "disabled", "unavailable", "rate", "age"}) {
        p.reason = reason; REQUIRE(T::DecodePacket(T::EncodePacket(p), false));
    }
    p.reason = "invented"; REQUIRE_FALSE(T::DecodePacket(T::EncodePacket(p), false));
}

TEST_CASE("teleport hello is additive and requires exact echo", "[oot_coop]") {
    Core::OotCoop::Hello h; h.client_id = "self"; h.build_id = std::string(40, 'A');
    auto ack = Core::OotCoop::EncodeHello(h); REQUIRE_FALSE(ack.contains("teleport"));
    ack["type"] = "hello_ack"; REQUIRE(Core::OotCoop::MatchesHello(ack, h));
    h.teleport = true; REQUIRE_FALSE(Core::OotCoop::MatchesHello(ack, h));
    ack["teleport"] = 1; REQUIRE(Core::OotCoop::MatchesHello(ack, h));
    ack["teleport"] = true; REQUIRE_FALSE(Core::OotCoop::MatchesHello(ack, h));
}

TEST_CASE("teleport readiness fails closed and settles against an anchor", "[oot_coop]") {
    auto k = Keys(); auto s = Sample(); T::History h;
    for (int i = 0; i < 3; ++i) h.Step(s, k);
    REQUIRE(T::Ready(s, h, k, false).empty());
    auto check = [&](auto mutate, std::string_view reason) {
        auto bad = s; mutate(bad); REQUIRE(T::Ready(bad, h, k, false) == reason);
    };
    check([](auto& b) { b.loading = true; }, "loading");
    check([](auto& b) { b.event_running = 5; }, "event");
    check([](auto& b) { b.block_bools_ok = false; }, "pause");
    check([](auto& b) { b.event_running.reset(); }, "unready");
    check([](auto& b) { b.block_bools_ok.reset(); }, "unready");
    check([](auto& b) { b.actor_pos.reset(); }, "unready");
    check([](auto& b) { b.command.reset(); }, "unready");
    check([](auto& b) { b.resident.reset(); }, "unready");
    check([](auto& b) { b.command = "AirA"; }, "state");
    check([](auto& b) { b.banc = "Cave_A"; }, "scene");
    check([](auto& b) { b.info_pos[0] = 10; }, "unready");
    k.enabled = false; REQUIRE(T::Ready(s, h, k, false) == "disabled"); k.enabled = true;
    k.deny_bancs = {s.banc}; REQUIRE(T::Ready(s, h, k, false) == "scene"); k.deny_bancs.clear();
    h.Reset(); REQUIRE(T::Ready(s, h, k, false) == "airborne");
    for (int i = 0; i < 10; ++i) {
        s.info_pos[1] += 0.006f; s.actor_pos = s.info_pos; h.Step(s, k);
        REQUIRE(T::Ready(s, h, k, false) == "airborne");
    }
    s = Sample(); s.command = "StandB"; h.Reset();
    for (int i = 0; i < 5; ++i) { // walking on level ground: horizontal motion keeps the settle count
        s.info_pos[0] += 0.5f; s.actor_pos = s.info_pos; h.Step(s, k);
    }
    REQUIRE(h.frames == 3); REQUIRE(T::Ready(s, h, k, true).empty());
    s = Sample(); s.command = "StandB";
    for (int i = 0; i < 3; ++i) h.Step(s, k);
    REQUIRE(T::Ready(s, h, k, true).empty());
    REQUIRE(T::Ready(s, h, k, false) == "state");
    s.resident->generation++; h.Step(s, k); REQUIRE(h.frames == 1);
}

TEST_CASE("teleport answers use the current frame sample and deduplicate", "[oot_coop]") {
    Rig r;
    auto request = Reply(); request.request = true;
    r.controller.Receive(request, r.now, r.port.log);
    r.controller.Receive(request, r.now, r.port.log);
    r.sample.yaw = 123; // after the network request; no cached pose exists in the controller
    r.Frame();
    auto output = r.controller.TakeOutput(); REQUIRE(output.size() == 1);
    REQUIRE(output[0].ok); REQUIRE(output[0].pos == r.sample.info_pos); REQUIRE(output[0].yaw == 123);
    ++request.id; r.controller.Receive(request, r.now, r.port.log);
    r.sample.event_running = 99; r.Frame(); output = r.controller.TakeOutput();
    REQUIRE(output.size() == 1); REQUIRE(output[0].reason == "event");
    ++request.id; r.controller.Receive(request, r.now, r.port.log); r.now += 2001; r.Frame();
    REQUIRE(r.controller.TakeOutput()[0].reason == "unready"); REQUIRE(r.calls.empty());
}

TEST_CASE("teleport calls once with the native matrix then verifies and settles", "[oot_coop]") {
    Rig r; r.Request(); r.Receive();
    REQUIRE(r.Frame()); REQUIRE(r.controller.Calling()); REQUIRE(r.calls.size() == 1);
    const auto& call = r.calls[0];
    REQUIRE(call.entry == 0x4000); REQUIRE(call.arguments[0] == r.sample.resident->actor);
    REQUIRE(call.arguments[1] == call.stack + 0x40); REQUIRE(call.arguments[2] == 0);
    auto matrix = Core::OotCoop::YawMatrix(Reply().pos, Reply().yaw);
    for (size_t i = 0; i < 6; ++i) {
        const auto [address, value] = call.stores[i + 4];
        REQUIRE(address == call.stack + 0x40 + i * 8); REQUIRE(address + 8 <= call.stack + 0x100);
        REQUIRE(static_cast<u32>(value) == std::bit_cast<u32>(matrix[i * 2]));
        REQUIRE(static_cast<u32>(value >> 32) == std::bit_cast<u32>(matrix[i * 2 + 1]));
    }
    r.controller.Returned(0x1234, r.port.log);
    r.Frame(); REQUIRE_FALSE(r.Logged("teleport arrived"));
    r.sample.info_pos = Reply().pos; r.Frame(); REQUIRE_FALSE(r.Logged("teleport arrived"));
    r.sample.actor_pos = Reply().pos; r.Frame(); REQUIRE(r.Logged("teleport arrived"));
    for (int i = 0; i < 60; ++i) r.Frame(); REQUIRE(r.Logged("+1s stable"));
    r.sample.info_pos[1] -= 5; r.sample.actor_pos = r.sample.info_pos;
    for (int i = 0; i < 120; ++i) r.Frame(); REQUIRE(r.Logged("+3s moved"));
    REQUIRE(r.calls.size() == 1); REQUIRE_FALSE(r.controller.Busy());
}

TEST_CASE("teleport refuses unsafe destinations without a guest call", "[oot_coop]") {
    for (int scenario = 0; scenario < 12; ++scenario) {
        CAPTURE(scenario);
        Rig r; r.Request(); auto p = Reply();
        if (scenario == 0) p.banc = "Field_B";
        if (scenario == 1) p.child = false;
        if (scenario == 2) { p.ok = false; p.reason = "busy"; }
        if (scenario == 3) r.sample.event_running = 10;
        if (scenario == 4) r.sample.command = "AirA";
        if (scenario == 5) { r.sample.info_pos[1]++; r.sample.actor_pos = r.sample.info_pos; }
        if (scenario == 6) r.valid = false;
        if (scenario == 7) r.valid_at_call = false; // generation changed between check and call
        if (scenario == 8) r.sample.loading = true;
        if (scenario == 9) r.sample.block_bools_ok = false;
        if (scenario == 10) p.room = 0;
        r.Receive(p);
        if (scenario == 11) r.now += 501;
        r.Frame(); REQUIRE(r.calls.empty()); REQUIRE_FALSE(r.controller.Busy()); REQUIRE(r.Logged("teleport refused"));
    }
}

TEST_CASE("teleport drops wrong replies expires waits and rejects busy triggers", "[oot_coop]") {
    Rig r; r.Request(); REQUIRE_FALSE(r.controller.Trigger("peer-b", r.now, r.port.log)); REQUIRE(r.Logged("busy"));
    r.Receive(Reply(2)); auto p = Reply(); p.peer = "peer-b"; r.Receive(p); r.Frame(); REQUIRE(r.calls.empty());
    r.now += 5001; r.Receive(); r.controller.Tick(r.now, r.port.log); r.Frame();
    REQUIRE(r.calls.empty()); REQUIRE_FALSE(r.controller.Busy()); REQUIRE(r.Logged("reply timeout"));
    r.Request(); r.Receive(Reply(2)); r.Frame(false); REQUIRE(r.calls.empty());
    r.now += 501; r.Frame(); REQUIRE(r.calls.empty()); REQUIRE(r.Logged("stale answer"));
}

TEST_CASE("teleport verification failure never retries and invalidation preserves return", "[oot_coop]") {
    for (int scenario = 0; scenario < 5; ++scenario) {
        Rig r; r.Request(); r.Receive(); r.Frame(); REQUIRE(r.calls.size() == 1);
        if (scenario == 0) {
            r.controller.Invalidate(r.port.log, "binding lost"); REQUIRE(r.controller.Calling());
        }
        r.controller.Returned(0, r.port.log);
        if (scenario == 1) ++r.sample.resident->generation;
        if (scenario == 2) r.sample.loading = true;
        if (scenario == 3) r.sample.child = false;
        for (int i = 0; i < 10; ++i) r.Frame();
        REQUIRE(r.Logged("teleport failed")); REQUIRE_FALSE(r.controller.Busy()); REQUIRE(r.calls.size() == 1);
        r.Receive(); r.Frame(); REQUIRE(r.calls.size() == 1);
    }
}

TEST_CASE("teleport trigger names prefixes any and ambiguity", "[oot_coop]") {
    const std::vector<T::Peer> peers{{"aaaa-1111", "Player One"}, {"bbbb-2222", "Player Two"}};
    REQUIRE(T::ResolveTrigger("teleport Player One", peers) == "aaaa-1111");
    REQUIRE(T::ResolveTrigger("teleport bbbb", peers) == "bbbb-2222");
    REQUIRE_FALSE(T::ResolveTrigger("teleport aaa", peers));
    REQUIRE_FALSE(T::ResolveTrigger("teleport any", peers));
    REQUIRE(T::ResolveTrigger("teleport any", {peers[0]}) == "aaaa-1111");
    REQUIRE_FALSE(T::ResolveTrigger("teleport any", {}));
    REQUIRE_FALSE(T::ResolveTrigger("teleport Player One", {{"aaaa-1", "Player One"}, {"aaaa-2", "Player One"}}));
    REQUIRE_FALSE(T::ResolveTrigger("teleport aaaa", {{"aaaa-1", "A"}, {"aaaa-2", "B"}}));
    REQUIRE_FALSE(T::ResolveTrigger("other aaaa", peers));
}

namespace {
struct Memory {
    std::array<u8, 0x20000> bytes{};
    bool IsValidVirtualAddressRange(u64 a, u64 n) const { return a && a <= bytes.size() && n <= bytes.size() - a; }
    u8 Read8(u64 a) const { return bytes.at(a); }
    u16 Read16(u64 a) const { u16 v; std::memcpy(&v, bytes.data() + a, 2); return v; }
    u32 Read32(u64 a) const { u32 v; std::memcpy(&v, bytes.data() + a, 4); return v; }
    u64 Read64(u64 a) const { u64 v; std::memcpy(&v, bytes.data() + a, 8); return v; }
    void Put(u64 a, u64 v, size_t n = 8) { std::memcpy(bytes.data() + a, &v, n); }
    void Write16(u64 a, u16 v) { Put(a, v, 2); }
    void Text(u64 a, const char* v) { std::memcpy(bytes.data() + a, v, std::strlen(v) + 1); }
    void Float(u64 a, float f) { Put(a, std::bit_cast<u32>(f), 4); }
};
struct MemoryRig {
    Memory memory;
    Core::OotGameDataTrace::Profile profile;
    std::shared_ptr<T::Keys> keys = std::make_shared<T::Keys>(Keys());
    MemoryRig() {
        auto a = std::make_shared<Core::OotCoop::Anim::Keys>();
        a->player = Core::OotCoop::Player::DecodeKeys({{"user_cell", 8}, {"user_equipment", 16}, {"owner", 24},
            {"holder", 32}, {"equipment", 40}, {"actor_record", 48}, {"actor_id", 56}, {"record_actor", 8},
            {"record_id", 16}, {"actor_name", 64}, {"lifecycle", 72}, {"lifecycle_lo", 3}, {"lifecycle_hi", 5},
            {"names", {"SyntheticPlayer"}}});
        REQUIRE(a->player);
        auto& m = memory;
        m.Put(0x108, 0x200); m.Put(0x210, 0x300); m.Put(0x318, 0x400);
        m.Put(0x420, 0x600); m.Put(0x628, 0x300); m.Put(0x430, 0x800); m.Put(0x438, 9, 4);
        m.Put(0x808, 0x400); m.Put(0x810, 9, 4); m.Put(0x440, 0xA00); m.Text(0xA00, "SyntheticPlayer"); m.Put(0x448, 1, 4);
        a->ctl_chain = {0x80}; a->ctl_count_off = 0; a->ctl_entries_off = 8; a->ctl_stride = 8;
        a->cmd_index_off = 0; a->cmd_name_off = 8; a->frame_off = 16; a->rate_off = 20;
        a->cmd_table[0] = {"StandA"}; a->cmd_table[1] = {"StandA"};
        m.Put(0x480, 0x2200); m.Put(0x2200, 1, 4); m.Put(0x2208, 0x2400); m.Put(0x2400, 0x2600);
        m.Put(0x2608, 0x2800); m.Text(0x2800, "StandA"); m.Float(0x2610, 1); m.Float(0x2614, 1);
        profile.anim = a; profile.teleport = keys; keys->block_bools = {3, 4};
        profile.event_got = 0x20; m.Put(0x120, 0xB00); m.Put(0xB00, 0xC00);
        profile.ghost.playing_info_cell = 0x28; m.Put(0x128, 0x3000);
        profile.ghost.pos = 0x20; profile.ghost.rot = 0x40;
        profile.ghost.banc_hash = 5; profile.ghost.child_hash = 1; profile.ghost.loading_hash = 2;
        profile.ghost.str64 = {0x100, 0x18, 0};
        // Synthetic flat tables: Bool slots 1..4 and one String64 slot 5.
        m.Put(0x10010, 4, 4); m.Put(0x10018, 0x11000); m.Put(0x10020, 0x12000); m.Put(0x1002C, 16, 4);
        for (u32 i = 1; i <= 4; ++i) { m.Put(0x12000 + i * 8, i, 4); m.Put(0x12004 + i * 8, i - 1, 4); }
        m.Put(0x11016, 1, 1);
        m.Put(0x10100, 1, 4); m.Put(0x10108, 0x13000); m.Put(0x10110, 0x14000); m.Put(0x1011C, 16, 4);
        m.Put(0x14028, 5, 4); m.Put(0x1402C, 0, 4); m.Put(0x13000, 0x15000); m.Text(0x15000, "Field_A");
        SetPosition({1, 2, 3}); m.Float(0x3060, 1);
    }
    void SetPosition(T::Vec p) {
        for (size_t i = 0; i < 3; ++i) { memory.Float(0x3020 + 4 * i, p[i]); memory.Float(0x500 + 4 * i, p[i]); }
    }
    T::Sample Read() { return T::ReadSample(memory, 0x100, 0x10000, profile); }
};
struct Backend {
    struct Context { u64 cookie{42}; };
    bool ready{true}, verified{true}, redirect{true};
    std::function<void()> after_capture = [] {};
    std::optional<Core::OotCoop::ApplyTarget> call;
    std::optional<u64> VerifiedEntry(const Core::OotGameDataTrace::Entry&) { return verified ? std::optional<u64>{0x4000} : std::nullopt; }
    bool CallReady() { return ready; }
    std::optional<Context> Capture() { after_capture(); return Context{}; }
    bool Redirect(const Core::OotCoop::ApplyTarget& t) { if (!redirect) return false; call = t; return true; }
};
}

TEST_CASE("teleport memory sample uses GOT chain source command and live positions", "[oot_coop]") {
    MemoryRig r;
    auto sample = r.Read(); REQUIRE(sample.actor_pos); REQUIRE(sample.resident);
    REQUIRE(sample.event_running == 0); REQUIRE(sample.block_bools_ok == true);
    REQUIRE(sample.command == "StandA"); REQUIRE(sample.banc == "Field_A"); REQUIRE(sample.child);
    REQUIRE(sample.info_pos == T::Vec{1, 2, 3}); REQUIRE(*sample.actor_pos == sample.info_pos);
    r.SetPosition({4, 5, 6}); sample = r.Read(); REQUIRE(sample.info_pos == T::Vec{4, 5, 6});
    r.memory.Put(0xC20, 0xE00); REQUIRE(r.Read().event_running == 0xE00);
    r.memory.Put(0x11046, 1, 1); REQUIRE(r.Read().block_bools_ok == false);
    r.memory.Put(0x11046, 2, 1); REQUIRE_FALSE(r.Read().block_bools_ok);
    r.memory.Put(0x810, 10, 4); REQUIRE_FALSE(r.Read().resident); REQUIRE_FALSE(r.Read().actor_pos);
    r.memory.Put(0xB00, 0xFFFFFF); REQUIRE_FALSE(r.Read().event_running);
}

TEST_CASE("teleport call adapter verifies entry memory resident and optional fall owner", "[oot_coop]") {
    MemoryRig r; Backend b;
    const auto resident = r.Read().resident; REQUIRE(resident);
    std::optional<Backend::Context> saved; u64 stack{};
    auto issue = [&] { return T::Issue(r.memory, b, r.profile, 0x100, 0x7000, 0x8000, *resident, Reply(), saved, stack); };
    SECTION("ordinary call writes no actor memory") {
        const auto before = r.memory.bytes;
        REQUIRE(issue()); REQUIRE(saved->cookie == 42); REQUIRE(stack == 0x6F00);
        REQUIRE(b.call->arguments[1] == stack + 0x40); REQUIRE(r.memory.bytes == before);
        REQUIRE_FALSE(issue()); // saved context cannot be overwritten
    }
    SECTION("entry mismatch") { b.verified = false; REQUIRE_FALSE(issue()); }
    SECTION("call memory unavailable") { b.ready = false; REQUIRE_FALSE(issue()); }
    SECTION("generation changes during capture") {
        b.after_capture = [&] { r.memory.Put(0x810, 10, 4); };
        REQUIRE_FALSE(issue()); REQUIRE_FALSE(b.call); REQUIRE_FALSE(saved);
    }
    SECTION("fall origin") {
        r.keys->fall_origin = T::FallOrigin{{32, 48}, 8, 16, 1};
        r.memory.Put(0x630, 0xD00); r.memory.Put(0xD08, resident->actor); r.memory.Write16(0xD10, 0x80);
        SECTION("OR preserves other bits") { REQUIRE(issue()); REQUIRE(r.memory.Read16(0xD10) == 0x81); }
        SECTION("wrong owner refuses without write") {
            r.memory.Put(0xD08, resident->actor + 8); REQUIRE_FALSE(issue()); REQUIRE(r.memory.Read16(0xD10) == 0x80);
        }
        SECTION("redirect refusal rolls back flag") {
            b.redirect = false; REQUIRE_FALSE(issue()); REQUIRE(r.memory.Read16(0xD10) == 0x80); REQUIRE_FALSE(saved);
        }
    }
}


TEST_CASE("teleport trigger file reads complete append-only lines without replay", "[oot_coop]") {
    const auto dir = std::filesystem::temp_directory_path() /
        ("oot-coop-trigger-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    REQUIRE(std::filesystem::create_directory(dir));
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); } } cleanup{dir};
    const auto path = dir / "trigger.txt";
    { std::ofstream f{path}; f << "teleport stale\n"; }
    T::TriggerFile file{path.string()};
    REQUIRE(file.Poll(1000).empty());
    { std::ofstream f{path, std::ios::app}; f << "teleport Player One"; }
    REQUIRE(file.Poll(1250).empty());
    { std::ofstream f{path, std::ios::app}; f << "\nteleport any\n"; }
    REQUIRE(file.Poll(1300).empty()); // four polls per second
    REQUIRE(file.Poll(1500) == std::vector<std::string>{"teleport Player One", "teleport any"});
    REQUIRE(file.Poll(1750).empty());
    { std::ofstream f{path, std::ios::app}; f << std::string(257, 'x') << "\nteleport aaaa\n"; }
    REQUIRE(file.Poll(2000) == std::vector<std::string>{"teleport aaaa"});
}

TEST_CASE("teleport list limits and frame request queue are bounded", "[oot_coop]") {
    auto j = KeyJson();
    for (auto name : {"allow_bancs", "deny_bancs", "requester_commands", "answer_commands"}) {
        auto v = j; v[name] = Json::array();
        for (int i = 0; i < 128; ++i) v[name].push_back("Name_" + std::to_string(i));
        REQUIRE(T::DecodeKeys(v)); v[name].push_back("Over"); REQUIRE_FALSE(T::DecodeKeys(v));
    }
    j["block_bools"] = std::vector<int>(32, 1); REQUIRE(T::DecodeKeys(j));
    j["block_bools"].push_back(1); REQUIRE_FALSE(T::DecodeKeys(j));
    Rig r;
    for (u64 n = 1; n <= 40; ++n) { auto req = Reply(n); req.request = true; r.Receive(req); }
    r.Frame(); REQUIRE(r.controller.TakeOutput().size() == 16);
    auto req = Reply(1); req.request = true; r.Receive(req); r.Frame(); REQUIRE(r.controller.TakeOutput().empty());
    r.controller.Invalidate(r.port.log, "binding lost");
    r.Receive(req); r.Frame(); REQUIRE(r.controller.TakeOutput().empty()); // preserve dedup across save epochs
}

TEST_CASE("teleport source replacement and peer loss cancel before issue", "[oot_coop]") {
    Rig r; r.Request(); ++r.sample.resident->generation;
    for (int i = 0; i < 3; ++i) r.Frame();
    r.Receive(); r.Frame(); REQUIRE(r.calls.empty()); REQUIRE(r.Logged("source scene/age/actor changed"));
    r.Request(); r.Receive(Reply(2)); r.controller.PeerGone("peer-a", r.port.log);
    r.Frame(); REQUIRE(r.calls.empty()); REQUIRE_FALSE(r.controller.Busy());
}
