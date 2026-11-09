// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/arm/dynarmic/oot_coop_apply.h"
#include "core/arm/dynarmic/oot_coop_ghost.h"
#include "core/arm/dynarmic/oot_coop_sync.h"

// Synthetic offsets and names only: nothing here is a value from the game.
namespace {
namespace Coop = Core::OotCoop;
namespace Trace = Core::OotGameDataTrace;
using Coop::Pose;
using Json = nlohmann::json;

constexpr char Build[] = "0123456789ABCDEF0123456789ABCDEF01234567";

Trace::GhostOffsets Offsets() {
    Trace::GhostOffsets g;
    g.playing_info_cell = 0x300000;
    g.pos = 0x100;
    g.rot = 0x10C;
    g.actor_mgr_cell = 0x300008;
    g.scale_const = 0x300010;
    g.banc_hash = 0x1111;
    g.child_hash = 0x2222;
    g.loading_hash = 0x3333;
    g.str64 = {0x80, 0x20, 0x8};
    g.create = {0x400000, {0xD503237F}};
    g.get = {0x401000, {0xD503237F}};
    g.release = {0x402000, {0xD503237F}};
    g.set_mtx = {0x403000, {0xD503237F}};
    g.actor_fields = {0x40, 0x48};
    g.link = {0x50, 0x18, 0x40, 0x48};
    g.actor_child = "Puppet_Child";
    g.actor_adult = "Puppet_Adult";
    return g;
}

struct Bytes {
    std::map<u64, u8> b;
    bool IsValidVirtualAddressRange(u64 address, size_t size) const {
        for (size_t i = 0; i < size; ++i)
            if (!b.contains(address + i)) return false;
        return true;
    }
    u8 Read8(u64 a) const { return b.at(a); }
    u32 Read32(u64 a) const {
        u32 v = 0;
        for (int i = 3; i >= 0; --i) v = (v << 8) | b.at(a + i);
        return v;
    }
    u64 Read64(u64 a) const { return u64{Read32(a)} | (u64{Read32(a + 4)} << 32); }
    void Put(u64 a, const void* data, size_t n) {
        const auto* p = static_cast<const u8*>(data);
        for (size_t i = 0; i < n; ++i) b[a + i] = p[i];
    }
    void P32(u64 a, u32 v) { Put(a, &v, 4); }
    void P64(u64 a, u64 v) { Put(a, &v, 8); }
    void PF(u64 a, float v) { Put(a, &v, 4); }
    float F(u64 a) const {
        const u32 bits = Read32(a);
        float f;
        std::memcpy(&f, &bits, 4);
        return f;
    }
};

struct Port {
    Bytes& m;
    struct Call {
        u64 entry;
        std::array<u64, 8> args;
    };
    std::vector<Call> calls;
    bool refuse{};
    std::optional<u64> R64(u64 a) {
        if (!m.IsValidVirtualAddressRange(a, 8)) return {};
        return m.Read64(a);
    }
    std::optional<u32> R32(u64 a) {
        if (!m.IsValidVirtualAddressRange(a, 4)) return {};
        return m.Read32(a);
    }
    bool Write(u64 a, const void* d, size_t n) {
        m.Put(a, d, n);
        return true;
    }
    std::optional<u64> Verified(const Trace::Entry& e) { return 0x10000000 + e.offset; }
    bool Call(u64 entry, const std::array<u64, 8>& args) {
        if (refuse) return false;
        calls.push_back({entry, args});
        return true;
    }
};

constexpr u64 Base = 0x10000000;
constexpr u64 Block = 0x20000400;
const auto NoLog = [](std::string_view) {};

Pose At(std::string banc, float x, float z, float yaw = 0, bool loading = false) {
    Pose p;
    p.banc = std::move(banc);
    p.child = true;
    p.loading = loading;
    p.pos = {x, 10.0f, z};
    p.yaw = yaw;
    return p;
}
} // namespace

TEST_CASE("OoT co-op v3 pose packets are bounded and never grants", "[core][oot_coop]") {
    REQUIRE(Coop::ProtocolVersion == 3);
    Pose pose = At("Forest_A", -46.09f, 15.39f, 108.6f);
    pose.t = 123456;
    const auto packet = Coop::EncodePose(pose);
    REQUIRE(Coop::EncodeFrame(packet).size() <= Coop::PoseFrameLimit);
    const auto client = Coop::DecodePose(packet);
    REQUIRE(client);
    REQUIRE(client->banc == "Forest_A");
    REQUIRE(std::fabs(client->pos[0] + 46.09f) < 0.01f);
    REQUIRE(std::fabs(client->yaw - 108.6f) < 0.01f);
    std::string origin;
    REQUIRE_FALSE(Coop::DecodePose(packet, &origin)); // a forwarded pose names its origin
    auto forwarded = packet;
    forwarded["origin"] = "peer-b";
    REQUIRE(Coop::DecodePose(forwarded, &origin));
    REQUIRE(origin == "peer-b");
    REQUIRE_FALSE(Coop::DecodePose(forwarded)); // the client form never carries one
    const auto rejects = [&](const char* field, Json value) {
        auto bad = forwarded;
        bad[field] = std::move(value);
        std::string o;
        return !Coop::DecodePose(bad, &o);
    };
    REQUIRE(rejects("banc", "Bad-Name"));
    REQUIRE(rejects("banc", std::string(64, 'a')));
    REQUIRE_FALSE(rejects("banc", ""));                 // no save loaded
    REQUIRE(rejects("pos", Json::array({1, 2})));
    REQUIRE(rejects("pos", Json::array({1, 2, 100001})));
    REQUIRE(rejects("pos", Json::array({1, true, 3})));
    REQUIRE(rejects("pos", Json::array({1, std::nan(""), 3})));
    REQUIRE(rejects("yaw", 400));
    REQUIRE(rejects("child", 1));
    REQUIRE(rejects("t", -1));
    REQUIRE(rejects("t", u64{1} << 32));
    REQUIRE(rejects("seq", 1));
    REQUIRE(rejects("origin", "has space"));
    REQUIRE(rejects("anim", "jump"));
    REQUIRE(rejects("anim_seq", -1));
    REQUIRE(rejects("rate", 11));
    REQUIRE(rejects("phase", 1.0));
    REQUIRE(rejects("phase", -0.1));
    auto no_anim = forwarded;
    no_anim.erase("anim");
    REQUIRE_FALSE(Coop::DecodePose(no_anim, &origin));
    // Never a grant, never state, never applied.
    REQUIRE_FALSE(Coop::DecodeChange(forwarded));
    const auto classes = Coop::DecodeAllowClasses("bool,pose");
    REQUIRE(classes);
    REQUIRE(classes->contains(Coop::Kind::Pose));
    REQUIRE_FALSE(Coop::WireKind(Coop::Kind::Pose));
    Coop::ApplyPolicy policy{true, {Coop::Kind::Pose}, {{Coop::Kind::Pose, 1}}, 0};
    REQUIRE_FALSE(policy.Allows({Coop::Kind::Pose, "a", 1, 1, 1, 0}));
}

TEST_CASE("OoT co-op ghost interpolation, matrix and staleness", "[core][oot_coop]") {
    const auto mid = Coop::Blend(At("S", 0, 0, 170), At("S", 2, 4, -170), 0.5f);
    REQUIRE(std::fabs(mid.pos[0] - 1.0f) < 1e-5f);
    REQUIRE(std::fabs(mid.pos[2] - 2.0f) < 1e-5f);
    REQUIRE(std::fabs(std::fabs(mid.yaw) - 180.0f) < 1e-3f); // across the seam, not through 0
    const auto m = Coop::YawMatrix({1, 2, 3}, 90.0f);
    REQUIRE(std::fabs(m[2] - 1.0f) < 1e-6f);  // m02 = sin
    REQUIRE(std::fabs(m[8] + 1.0f) < 1e-6f);  // m20 = -sin
    REQUIRE(std::fabs(m[10]) < 1e-6f);        // m22 = cos
    REQUIRE(m[3] == 1.0f);
    REQUIRE(m[7] == 2.0f);
    REQUIRE(m[11] == 3.0f);
    REQUIRE(std::fabs(std::atan2(m[2], m[10]) * 180.0 / 3.141592653589793 - 90.0) < 1e-3);
    Coop::PeerTrack track;
    track.Add(1000, At("S", 0, 0));
    track.Add(1100, At("S", 10, 0));
    REQUIRE(std::fabs(track.At(1150)->pos[0] - 5.0f) < 1e-4f); // drawn 100 ms behind
    REQUIRE(track.At(1300)->pos[0] == 10.0f);                    // never extrapolated
    REQUIRE_FALSE(track.At(1100 + 5001));                        // stale
    track.Add(1200, At("T", 50, 0));
    REQUIRE(track.At(1250)->banc == "T"); // a scene change is never blended
    track.online = false;
    REQUIRE_FALSE(track.At(1250));
}

TEST_CASE("OoT co-op pose sender paces, keeps alive and settles after a scene change",
          "[core][oot_coop]") {
    Coop::PoseSender sender;
    REQUIRE(sender.Sample(At("A", 0, 0), 1000));
    REQUIRE_FALSE(sender.Sample(At("A", 0.05f, 0), 1030)); // 15 Hz pacing
    REQUIRE(sender.Sample(At("A", 0.05f, 0), 1070));
    REQUIRE_FALSE(sender.Sample(At("A", 0.05f, 0), 1500)); // unchanged
    REQUIRE(sender.Sample(At("A", 0.05f, 0), 2100));        // 1 Hz keepalive
    // A new scene keeps saying loading until the position moves away from the old one.
    auto same = sender.Sample(At("B", 0.05f, 0), 2200);
    REQUIRE(same);
    REQUIRE(same->loading);
    same = sender.Sample(At("B", 0.05f, 0), 2300);
    REQUIRE((!same || same->loading));
    const auto moved = sender.Sample(At("B", 40, 9), 2400);
    REQUIRE(moved);
    REQUIRE_FALSE(moved->loading);
}

TEST_CASE("OoT co-op pose animation hint: category from speed, sequence only on change",
          "[core][oot_coop]") {
    Coop::PoseSender sender;
    std::optional<Pose> p = sender.Sample(At("A", 0, 0), 1000);
    REQUIRE(p->anim == "idle");
    const u32 seq0 = p->anim_seq;
    p = sender.Sample(At("A", 0.15f, 0), 1100);          // 1.5 m/s
    REQUIRE(p->anim == "walk");
    REQUIRE(p->anim_seq == seq0 + 1);
    REQUIRE(std::fabs(p->rate - 1.0f) < 0.05f);
    p = sender.Sample(At("A", 0.30f, 0), 1200);          // still walking: same sequence
    REQUIRE(p->anim_seq == seq0 + 1);
    REQUIRE(p->phase > 0.0f);
    REQUIRE(p->phase < 1.0f);
    p = sender.Sample(At("A", 0.75f, 0), 1300);          // 4.5 m/s
    REQUIRE(p->anim == "run");
    REQUIRE(p->anim_seq == seq0 + 2);
    const auto packet = Coop::EncodePose(*p);
    REQUIRE(Coop::DecodePose(packet)->anim == "run");
}

TEST_CASE("OoT co-op reads the local pose from the profile's fields only", "[core][oot_coop]") {
    const auto g = Offsets();
    Bytes m;
    const u64 manager = 0x50000000, info = 0x51000000;
    m.P64(Base + g.playing_info_cell, info);
    m.PF(info + g.pos, 1.5f);
    m.PF(info + g.pos + 4, 2.5f);
    m.PF(info + g.pos + 8, -3.5f);
    for (u64 i = 0; i < 9; ++i) m.PF(info + g.rot + i * 4, 0.0f);
    m.PF(info + g.rot + 8, 1.0f); // m02: facing +X
    // Flat Bool table at manager+0x10 and a String64-like table at manager+0x80.
    const auto table = [&](u64 at, u64 data, u64 hashes, std::vector<std::pair<u32, u32>> keys, u32 count) {
        m.P32(manager + at, count);
        m.P64(manager + at + 8, data);
        m.P64(manager + at + 0x10, hashes);
        m.P32(manager + at + 0x1C, 8);
        for (u32 s = 0; s < 8; ++s) m.P64(hashes + s * 8, 0);
        for (const auto& [key, index] : keys) {
            u32 s = key % 8;
            while (m.Read32(hashes + s * 8)) s = (s + 1) % 8;
            m.P32(hashes + s * 8, key);
            m.P32(hashes + s * 8 + 4, index);
        }
    };
    table(0x10, 0x52000000, 0x52100000, {{g.child_hash, 0}, {g.loading_hash, 1}}, 2);
    m.b[0x52000000 + 0x16] = 1;
    m.b[0x52000000 + 0x18 + 0x16] = 0;
    table(0x80, 0x53000000, 0x53100000, {{g.banc_hash, 0}}, 1);
    m.P64(0x53000000 + 8, 0x54000000);
    m.Put(0x54000000, "Forest_A", 9);
    auto pose = Coop::ReadLocalPose(m, Base, manager, g, 77);
    REQUIRE(pose.banc == "Forest_A");
    REQUIRE(pose.child);
    REQUIRE_FALSE(pose.loading);
    REQUIRE(pose.pos == std::array<float, 3>{1.5f, 2.5f, -3.5f});
    REQUIRE(std::fabs(pose.yaw - 90.0f) < 1e-4f);
    REQUIRE(pose.t == 77);
    m.b.erase(info + g.pos + 4); // unreadable: no scene, so nothing is drawn of us
    REQUIRE(Coop::ReadLocalPose(m, Base, manager, g, 78).banc.empty());
    REQUIRE(Coop::ReadLocalPose(m, Base, 0, g, 78).banc.empty());
}

TEST_CASE("OoT co-op ghost driver creates, moves, parks and drops without stale calls",
          "[core][oot_coop]") {
    const auto g = Offsets();
    Bytes m;
    Port port{m};
    m.P64(Base + g.actor_mgr_cell, 0x60000000);
    for (u64 i = 0; i < 3; ++i) m.PF(Base + g.scale_const + i * 4, 1.0f);
    Coop::GhostDriver<Port> ghost{g, Base};
    using State = Coop::GhostDriver<Port>::State;
    std::vector<std::string> lines;
    const auto log = [&](std::string_view line) { lines.emplace_back(line); };
    const Pose local = At("Forest_A", 0, 0);
    std::vector<Coop::PeerView> peers{{"peer-b", At("Forest_A", 3, 4, 45)}};

    // Not drawn while either side loads or the scenes differ.
    REQUIRE_FALSE(ghost.Frame(port, Block, At("Forest_A", 0, 0, 0, true), peers, log));
    REQUIRE_FALSE(ghost.Frame(port, Block, At("Other", 0, 0), peers, log));
    REQUIRE(port.calls.empty());

    // Create: x0 manager, x1 &name pointer, x2 &argument, x3 &handle, w4 1.
    REQUIRE(ghost.Frame(port, Block, local, peers, log));
    REQUIRE(port.calls.size() == 1);
    const auto create = port.calls.back();
    REQUIRE(create.entry == Base + g.create.offset);
    REQUIRE(create.args == std::array<u64, 8>{0x60000000, Block + 8, Block + 0x40, Block, 1, 0, 0, 0});
    REQUIRE(m.Read64(Block + 8) == Block + 0x100);
    std::string name;
    for (u64 i = 0; m.b.at(Block + 0x100 + i); ++i) name.push_back(static_cast<char>(m.b.at(Block + 0x100 + i)));
    REQUIRE(name == "Puppet_Child");
    REQUIRE(m.F(Block + 0x40) == 3.0f);
    REQUIRE(m.F(Block + 0x48) == 4.0f);
    REQUIRE(m.F(Block + 0x40 + 0x30) == 1.0f);
    REQUIRE(m.b.at(Block + 0x40 + 0x98) == 3);
    REQUIRE(ghost.Calling());
    REQUIRE_FALSE(ghost.Frame(port, Block, local, peers, log)); // one call in flight
    m.P64(Block, 0x61000000); // the manager's request record
    m.P32(0x61000000, 2);
    ghost.Returned(port, Block, 1, log);
    REQUIRE(ghost.Slots()[0].state == State::Pending);

    REQUIRE_FALSE(ghost.Frame(port, Block, local, peers, log)); // still loading the pack
    m.P32(0x61000000, 3);
    REQUIRE(ghost.Frame(port, Block, local, peers, log));
    REQUIRE(port.calls.back().entry == Base + g.get.offset);
    REQUIRE(port.calls.back().args[0] == Block);
    const u64 actor = 0x62000000;
    m.P64(actor, 0xABC0);
    m.P64(actor + 0x40, 0x1);
    m.P64(actor + 0x48, 0x2);
    const u64 record = 0x63000000;
    m.P64(actor + 0x50, record);
    m.P32(actor + 0x18, 0x35c);
    m.P64(record + 0x40, actor);
    m.P32(record + 0x48, 0x35c);
    ghost.Returned(port, Block, actor, log);
    REQUIRE(ghost.Slots()[0].state == State::Live);

    // Move into place: set-transform(actor, &matrix, 0) with the yaw matrix in the slot.
    REQUIRE(ghost.Frame(port, Block, local, peers, log));
    REQUIRE(port.calls.back().entry == Base + g.set_mtx.offset);
    REQUIRE(port.calls.back().args[0] == actor);
    REQUIRE(port.calls.back().args[1] == Block + 0x10);
    REQUIRE(m.F(Block + 0x10 + 3 * 4) == 3.0f);
    REQUIRE(m.F(Block + 0x10 + 11 * 4) == 4.0f);
    ghost.Returned(port, Block, 0, log);
    const size_t after_move = port.calls.size();
    REQUIRE_FALSE(ghost.Frame(port, Block, local, peers, log)); // unchanged: no call
    peers[0].pose->pos[0] = 3.5f;
    REQUIRE(ghost.Frame(port, Block, local, peers, log));
    ghost.Returned(port, Block, 0, log);
    REQUIRE(port.calls.size() == after_move + 1);

    // The peer leaves the scene: parked once below the map, then left alone.
    peers[0].pose->banc = "Elsewhere";
    REQUIRE(ghost.Frame(port, Block, local, peers, log));
    REQUIRE(m.F(Block + 0x10 + 7 * 4) == Coop::GhostDriver<Port>::ParkY);
    ghost.Returned(port, Block, 0, log);
    REQUIRE_FALSE(ghost.Frame(port, Block, local, peers, log));
    peers[0].pose->banc = "Forest_A";
    REQUIRE(ghost.Frame(port, Block, local, peers, log)); // shown again
    ghost.Returned(port, Block, 0, log);
    // A dead actor (scene unloaded under us: its record moved on to another generation) is
    // dropped without any call, even with the vtable still in place.
    m.P32(record + 0x48, 0x35d);
    const size_t before_drop = port.calls.size();
    REQUIRE_FALSE(ghost.Frame(port, Block, local, peers, log));
    REQUIRE(port.calls.size() == before_drop);
    REQUIRE(ghost.Slots()[0].state == State::None);
    // Re-created after the retry delay.
    REQUIRE(std::any_of(lines.begin(), lines.end(),
                        [](const std::string& l) { return l.find("dropped (actor no longer valid)") != std::string::npos; }));
    for (int i = 0; i < 60 && !ghost.Calling(); ++i) ghost.Frame(port, Block, local, peers, log);
    REQUIRE(ghost.Calling());
    REQUIRE(port.calls.back().entry == Base + g.create.offset);
}

TEST_CASE("OoT co-op ghost create failures release and back off", "[core][oot_coop]") {
    const auto g = Offsets();
    Bytes m;
    Port port{m};
    m.P64(Base + g.actor_mgr_cell, 0x60000000);
    for (u64 i = 0; i < 3; ++i) m.PF(Base + g.scale_const + i * 4, 1.0f);
    Coop::GhostDriver<Port> ghost{g, Base};
    std::vector<Coop::PeerView> peers{{"peer-b", At("Forest_A", 3, 4)}};
    const Pose local = At("Forest_A", 0, 0);
    REQUIRE(ghost.Frame(port, Block, local, peers, NoLog));
    m.P64(Block, 0);
    ghost.Returned(port, Block, 0, NoLog); // refused: no request record
    for (int i = 0; i < 100; ++i) REQUIRE_FALSE(ghost.Frame(port, Block, local, peers, NoLog));
    // Failed request (state 4): the record is released, never fetched.
    Coop::GhostDriver<Port> second{g, Base};
    REQUIRE(second.Frame(port, Block, local, peers, NoLog));
    m.P64(Block, 0x61000000);
    m.P32(0x61000000, 4);
    second.Returned(port, Block, 1, NoLog);
    REQUIRE(second.Frame(port, Block, local, peers, NoLog));
    REQUIRE(port.calls.back().entry == Base + g.release.offset);
    REQUIRE(port.calls.back().args[0] == Block);
    // A request that completes after our scene changed is released too.
    Coop::GhostDriver<Port> third{g, Base};
    REQUIRE(third.Frame(port, Block, local, peers, NoLog));
    m.P64(Block, 0x61000000);
    m.P32(0x61000000, 2);
    third.Returned(port, Block, 1, NoLog);
    m.P32(0x61000000, 3);
    REQUIRE(third.Frame(port, Block, At("Elsewhere", 0, 0), peers, NoLog));
    REQUIRE(port.calls.back().entry == Base + g.release.offset);
    // A refused redirect starts nothing.
    Coop::GhostDriver<Port> fourth{g, Base};
    port.refuse = true;
    REQUIRE_FALSE(fourth.Frame(port, Block, local, peers, NoLog));
    REQUIRE_FALSE(fourth.Calling());
}

TEST_CASE("OoT co-op ghost offsets load only from the private profile", "[core][oot_coop]") {
    Json ghost{{"playing_info_cell", "0x300000"}, {"pos", "0x100"}, {"rot", "0x10C"},
               {"actor_mgr_cell", "0x300008"}, {"scale_const", "0x300010"},
               {"banc_hash", "0x1111"}, {"child_hash", "0x2222"}, {"loading_hash", "0x3333"},
               {"str64", Json::array({"0x80", "0x20", "0x8"})},
               {"create", {{"at", "0x400000"}, {"words", Json::array({"0xD503237F"})}}},
               {"get", {{"at", "0x401000"}, {"words", Json::array({"0xD503237F"})}}},
               {"release", {{"at", "0x402000"}, {"words", Json::array({"0xD503237F"})}}},
               {"set_mtx", {{"at", "0x403000"}, {"words", Json::array({"0xD503237F"})}}},
               {"actor_fields", Json::array({"0x40", "0x48"})},
               {"link", Json::array({"0x50", "0x18", "0x40", "0x48"})},
               {"actor_child", "Puppet_Child"}};
    const auto load = [&](const Json& g) {
        return Coop::DecodeProfile(Json{{"offsets", {{Build, {{"manager_cell", "0x200000"}, {"ghost", g}}}}}}, Build);
    };
    const auto profile = load(ghost);
    REQUIRE(profile);
    REQUIRE(profile->ghost.Valid());
    REQUIRE(profile->ghost.actor_child == "Puppet_Child");
    REQUIRE(profile->ghost.actor_adult.empty()); // falls back to the child puppet
    REQUIRE(profile->ghost.str64 == std::array<u64, 3>{0x80, 0x20, 0x8});
    REQUIRE(profile->ghost.actor_fields == std::vector<u64>{0x40, 0x48});
    REQUIRE(profile->ghost.link == std::array<u64, 4>{0x50, 0x18, 0x40, 0x48});
    for (const char* missing : {"create", "set_mtx", "actor_fields", "link", "str64", "banc_hash"}) {
        auto bad = ghost;
        bad.erase(missing);
        REQUIRE_FALSE(load(bad));
    }
    auto bad = ghost;
    bad["actor_child"] = "../Pack";
    REQUIRE_FALSE(load(bad));
    bad = ghost;
    bad["extra"] = 1;
    REQUIRE_FALSE(load(bad));
    bad = ghost;
    bad["actor_fields"] = Json::array({"0x41"});
    REQUIRE_FALSE(load(bad));
    // Without a ghost object the profile still loads, with the class unavailable.
    const auto plain = Coop::DecodeProfile(Json{{"offsets", {{Build, {{"manager_cell", "0x200000"}}}}}}, Build);
    REQUIRE(plain);
    REQUIRE_FALSE(plain->ghost.Valid());
}

TEST_CASE("OoT co-op ghost drops an actor in teardown and logs the adult fallback", "[core][oot_coop]") {
    auto g = Offsets();
    g.actor_adult.clear();
    g.lifecycle = 0x2C;
    g.lifecycle_lo = 7;
    g.lifecycle_hi = 11;
    Bytes m;
    Port port{m};
    m.P64(Base + g.actor_mgr_cell, 0x60000000);
    for (u64 i = 0; i < 3; ++i) m.PF(Base + g.scale_const + i * 4, 1.0f);
    Coop::GhostDriver<Port> ghost{g, Base};
    using State = Coop::GhostDriver<Port>::State;
    std::vector<std::string> lines;
    const auto log = [&](std::string_view line) { lines.emplace_back(line); };
    const auto logged = [&](std::string_view what) {
        return std::any_of(lines.begin(), lines.end(),
                           [&](const std::string& l) { return l.find(what) != std::string::npos; });
    };
    const Pose local = At("Forest_A", 0, 0);
    auto adult = At("Forest_A", 3, 4, 45);
    adult.child = false;
    std::vector<Coop::PeerView> peers{{"peer-b", adult}};

    REQUIRE(ghost.Frame(port, Block, local, peers, log));
    std::string name;
    for (u64 i = 0; m.b.at(Block + 0x100 + i); ++i) name.push_back(static_cast<char>(m.b.at(Block + 0x100 + i)));
    REQUIRE(name == "Puppet_Child");
    m.P64(Block, 0x61000000);
    m.P32(0x61000000, 3);
    ghost.Returned(port, Block, 1, log);
    REQUIRE(logged("adult peer: the profile has no adult puppet"));
    REQUIRE(ghost.Frame(port, Block, local, peers, log)); // take
    const u64 actor = 0x62000000, record = 0x63000000;
    m.P64(actor, 0xABC0);
    m.P64(actor + 0x40, 0x1);
    m.P64(actor + 0x48, 0x2);
    m.P64(actor + 0x50, record);
    m.P32(actor + 0x18, 0x35c);
    m.P32(actor + 0x2C, 3);
    m.P64(record + 0x40, actor);
    m.P32(record + 0x48, 0x35c);
    ghost.Returned(port, Block, actor, log);
    REQUIRE(ghost.Slots()[0].state == State::Live);
    REQUIRE(ghost.Frame(port, Block, local, peers, log)); // move: lifecycle 3 is alive
    REQUIRE(port.calls.back().entry == Base + g.set_mtx.offset);
    ghost.Returned(port, Block, 0, log);
    // Teardown started (lifecycle in 7..11) while the link still names the actor: no call.
    peers[0].pose->pos[0] = 5.0f;
    m.P32(actor + 0x2C, 8);
    const size_t before = port.calls.size();
    REQUIRE_FALSE(ghost.Frame(port, Block, local, peers, log));
    REQUIRE(port.calls.size() == before);
    REQUIRE(ghost.Slots()[0].state == State::None);
    REQUIRE(logged("dropped (actor being deleted, lifecycle 8)"));
}

TEST_CASE("OoT co-op ghost lifecycle field is optional and bounded", "[core][oot_coop]") {
    Json ghost{{"playing_info_cell", "0x300000"}, {"pos", "0x100"}, {"rot", "0x10C"},
               {"actor_mgr_cell", "0x300008"}, {"scale_const", "0x300010"},
               {"banc_hash", "0x1111"}, {"child_hash", "0x2222"}, {"loading_hash", "0x3333"},
               {"str64", Json::array({"0x80", "0x20", "0x8"})},
               {"create", {{"at", "0x400000"}, {"words", Json::array({"0xD503237F"})}}},
               {"get", {{"at", "0x401000"}, {"words", Json::array({"0xD503237F"})}}},
               {"release", {{"at", "0x402000"}, {"words", Json::array({"0xD503237F"})}}},
               {"set_mtx", {{"at", "0x403000"}, {"words", Json::array({"0xD503237F"})}}},
               {"actor_fields", Json::array({"0x40", "0x48"})},
               {"link", Json::array({"0x50", "0x18", "0x40", "0x48"})},
               {"actor_child", "Puppet_Child"},
               {"lifecycle", Json::array({"0x2C", "0x7", "0xB"})}};
    const auto load = [&](const Json& g) {
        return Coop::DecodeProfile(Json{{"offsets", {{Build, {{"manager_cell", "0x200000"}, {"ghost", g}}}}}}, Build);
    };
    const auto profile = load(ghost);
    REQUIRE(profile);
    REQUIRE(profile->ghost.lifecycle == 0x2C);
    REQUIRE(profile->ghost.lifecycle_lo == 7);
    REQUIRE(profile->ghost.lifecycle_hi == 11);
    auto bad = ghost;
    bad["lifecycle"] = Json::array({"0x2C", "0xB", "0x7"}); // empty range
    REQUIRE_FALSE(load(bad));
    bad["lifecycle"] = Json::array({"0x2D", "0x7", "0xB"}); // misaligned
    REQUIRE_FALSE(load(bad));
    bad["lifecycle"] = Json::array({"0x2C", "0x7"});
    REQUIRE_FALSE(load(bad));
    bad.erase("lifecycle");
    const auto without = load(bad);
    REQUIRE(without);
    REQUIRE(without->ghost.lifecycle == 0);
}

TEST_CASE("OoT co-op ghost animation path plays a category's command once per change", "[core][oot_coop]") {
    auto g = Offsets();
    g.anim_play = {0x404000, {0xD503237F}};
    g.anim_idle = "Idle_Cmd";
    g.anim_walk = "Walk_Cmd";
    Bytes m;
    Port port{m};
    m.P64(Base + g.actor_mgr_cell, 0x60000000);
    for (u64 i = 0; i < 3; ++i) m.PF(Base + g.scale_const + i * 4, 1.0f);
    Coop::GhostDriver<Port> ghost{g, Base};
    const Pose local = At("Forest_A", 0, 0);
    std::vector<Coop::PeerView> peers{{"peer-b", At("Forest_A", 3, 4, 45)}};
    REQUIRE(ghost.Frame(port, Block, local, peers, NoLog));
    m.P64(Block, 0x61000000);
    m.P32(0x61000000, 3);
    ghost.Returned(port, Block, 1, NoLog);
    REQUIRE(ghost.Frame(port, Block, local, peers, NoLog));
    const u64 actor = 0x62000000, record = 0x63000000;
    m.P64(actor, 0xABC0);
    m.P64(actor + 0x40, 0x1);
    m.P64(actor + 0x48, 0x2);
    m.P64(actor + 0x50, record);
    m.P32(actor + 0x18, 0x35c);
    m.P64(record + 0x40, actor);
    m.P32(record + 0x48, 0x35c);
    ghost.Returned(port, Block, actor, NoLog);
    // First visible frame moves (shown), the next plays idle.
    REQUIRE(ghost.Frame(port, Block, local, peers, NoLog));
    REQUIRE(port.calls.back().entry == Base + g.set_mtx.offset);
    ghost.Returned(port, Block, 0, NoLog);
    REQUIRE(ghost.Frame(port, Block, local, peers, NoLog));
    REQUIRE(port.calls.back().entry == Base + g.anim_play.offset);
    const u64 req = port.calls.back().args[0];
    REQUIRE(m.Read64(req) == actor);
    std::string cmd;
    for (u64 a = m.Read64(req + 8); m.b.at(a); ++a) cmd.push_back(static_cast<char>(m.b.at(a)));
    REQUIRE(cmd == "Idle_Cmd");
    REQUIRE(m.F(req + 0x14) == 1.0f);
    REQUIRE(m.F(req + 0x18) == -1.0f);
    ghost.Returned(port, Block, 1, NoLog);
    const size_t n = port.calls.size();
    REQUIRE_FALSE(ghost.Frame(port, Block, local, peers, NoLog)); // same category: nothing
    REQUIRE(port.calls.size() == n);
    peers[0].pose->anim = "walk";
    peers[0].pose->anim_seq = 1;
    peers[0].pose->rate = 1.5f;
    REQUIRE(ghost.Frame(port, Block, local, peers, NoLog));
    REQUIRE(port.calls.back().entry == Base + g.anim_play.offset);
    REQUIRE(m.F(req + 0x14) == 1.5f);
    ghost.Returned(port, Block, 1, NoLog);
    // No command for "run": no call for it.
    peers[0].pose->anim = "run";
    peers[0].pose->anim_seq = 2;
    REQUIRE_FALSE(ghost.Frame(port, Block, local, peers, NoLog));
    // The key file switch: "anim" needs a play entry, names are checked.
    Json ghost_json{{"playing_info_cell", "0x300000"}, {"pos", "0x100"}, {"rot", "0x10C"},
                    {"actor_mgr_cell", "0x300008"}, {"scale_const", "0x300010"},
                    {"banc_hash", "0x1111"}, {"child_hash", "0x2222"}, {"loading_hash", "0x3333"},
                    {"str64", Json::array({"0x80", "0x20", "0x8"})},
                    {"create", {{"at", "0x400000"}, {"words", Json::array({"0xD503237F"})}}},
                    {"get", {{"at", "0x401000"}, {"words", Json::array({"0xD503237F"})}}},
                    {"release", {{"at", "0x402000"}, {"words", Json::array({"0xD503237F"})}}},
                    {"set_mtx", {{"at", "0x403000"}, {"words", Json::array({"0xD503237F"})}}},
                    {"actor_fields", Json::array({"0x40", "0x48"})},
                    {"link", Json::array({"0x50", "0x18", "0x40", "0x48"})},
                    {"actor_child", "Puppet_Child"},
                    {"anim", {{"play", {{"at", "0x404000"}, {"words", Json::array({"0xD503237F"})}}},
                              {"idle", "Idle_Cmd"}, {"walk", "Walk_Cmd"}}}};
    const auto load = [&](const Json& j) {
        return Coop::DecodeProfile(Json{{"offsets", {{Build, {{"manager_cell", "0x200000"}, {"ghost", j}}}}}}, Build);
    };
    const auto profile = load(ghost_json);
    REQUIRE(profile);
    REQUIRE(profile->ghost.anim_play.offset == 0x404000);
    REQUIRE(profile->ghost.anim_walk == "Walk_Cmd");
    REQUIRE(profile->ghost.anim_run.empty());
    auto bad = ghost_json;
    bad["anim"].erase("play");
    REQUIRE_FALSE(load(bad));
    bad = ghost_json;
    bad["anim"]["idle"] = "../x";
    REQUIRE_FALSE(load(bad));
    bad = ghost_json;
    bad["anim"]["frame"] = "x";
    REQUIRE_FALSE(load(bad));
}
