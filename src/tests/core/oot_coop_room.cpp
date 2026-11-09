// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <cmath>
#include <limits>
#include <catch2/catch_test_macros.hpp>
#include "core/arm/dynarmic/oot_coop_room.h"
#include "core/arm/dynarmic/oot_coop_sync.h"
#include "core/arm/dynarmic/oot_coop_ux.h"
#include "video_core/coop_overlay.h"

namespace {
namespace Room = Core::OotCoop::Room;
namespace Ux = Core::OotCoop::Ux;
namespace Overlay = VideoCore::CoopOverlay;
using Core::OotCoop::Kind;
using Json = nlohmann::json;
Json Presence(std::string id = "peer-a", bool online = true, bool finished = false) {
    Json state = Json::object();
    if (!online) state["online"] = false;
    if (finished) state["finished"] = true;
    return {{"type", "client_state"}, {"origin", id}, {"state", state}};
}
std::vector<std::string> Texts(const std::vector<Room::Notice>& notices) {
    std::vector<std::string> out;
    for (const auto& n : notices) if (!n.text.empty()) out.push_back(n.text);
    return out;
}
Json Minimap() {
    return {{"centre", {0.5, 0.5}}, {"half", {0.2, 0.1}}, {"world_centre", {0, 0}},
        {"scale", 0.25}, {"clip", "rectangle"}, {"centre_source", "fixed"},
        {"rotation", "north-up"}, {"yaw_sign", 1}, {"swap_xz", false}, {"x_sign", 1}, {"z_sign", 1}};
}
}
TEST_CASE("OotCoopRoom: presence transitions, duplicates and names", "[oot_coop]") {
    Room::PresenceBook b;
    CHECK(Texts(b.Ready(0)).empty());
    auto first = b.Receive(Presence(), "A\nB", 0);
    CHECK(Texts(first) == std::vector<std::string>{"A?B connected"});
    CHECK(first.back().log == "OoT co-op: presence peer-a A?B connected (toast)");
    CHECK(Texts(b.Receive(Presence(), "A\nB", 100)).empty());
    CHECK(Texts(b.Receive(Presence("peer-a", false), "A\nB", 10000)) ==
          std::vector<std::string>{"A?B disconnected"});
    CHECK(Texts(b.Receive(Presence("peer-a", false), "A\nB", 11000)).empty());
    CHECK(Texts(b.Receive(Presence(), "A\nB", 20000)) == std::vector<std::string>{"A?B reconnected"});
    CHECK(b.Peers().size() == 1);
    CHECK(b.Peers()[0].online);
}
TEST_CASE("OotCoopRoom: held transitions and bursts report final state", "[oot_coop]") {
    Room::PresenceBook b;
    b.Receive(Presence(), "Alpha", 0);
    const auto held = b.Receive(Presence("peer-a", false), "Alpha", 100);
    CHECK(Texts(held).empty());
    CHECK(held.back().log.ends_with("disconnected (held)"));
    CHECK(Texts(b.Tick(9999)).empty());
    CHECK(Texts(b.Tick(10000)) == std::vector<std::string>{"Alpha disconnected"});
    CHECK(Texts(b.Tick(20000)).empty());
    CHECK(Texts(b.Receive(Presence(), "Alpha", 20001)) == std::vector<std::string>{"Alpha reconnected"});
    b.Receive(Presence("peer-a", false), "Alpha", 20100);
    b.Receive(Presence(), "Alpha", 20200);
    b.Receive(Presence("peer-a", false), "Alpha", 20300);
    CHECK(Texts(b.Tick(30001)) == std::vector<std::string>{"Alpha disconnected"});
    CHECK(Texts(b.Tick(40001)).empty());
    b.Receive(Presence(), "Alpha", 40002);
    b.Receive(Presence("peer-a", false), "Alpha", 40003);
    b.Receive(Presence(), "Alpha", 40004);
    CHECK(Texts(b.Tick(50002)).empty()); // a flap ending at the toasted state is silent
}
TEST_CASE("OotCoopRoom: our own reconnect preserves presence and reconciles missing peers", "[oot_coop]") {
    Room::PresenceBook b;
    CHECK(Texts(b.Lost(0)).empty()); // failed initial connection
    b.Ready(0);
    b.Receive(Presence("peer-a"), "Alpha", 0);
    b.Receive(Presence("peer-b"), "Beta", 0);
    b.Pose("peer-a", "SyntheticArea", true, false, 10);
    CHECK(Texts(b.Lost(20000)) == std::vector<std::string>{"Relay connection lost"});
    CHECK(Texts(b.Lost(21000)).empty());
    CHECK(Texts(b.Tick(50000)).empty());
    CHECK(Texts(b.Ready(60000)) == std::vector<std::string>{"Reconnected to relay"});
    CHECK(Texts(b.Receive(Presence("peer-a"), "Alpha", 60001)).empty());
    CHECK(Texts(b.Tick(64999)).empty());
    CHECK(Texts(b.Tick(65000)) == std::vector<std::string>{"Beta disconnected"});
    CHECK(b.Peers()[0].banc == "SyntheticArea");
    CHECK(b.Peers()[0].online);
    CHECK_FALSE(b.Peers()[1].online);
    CHECK(Texts(b.Tick(75000)).empty());
    // Another relay outage restarts the grace interval, including through repeated failures.
    b.Lost(80000); b.Ready(81000); b.Lost(82000);
    CHECK(Texts(b.Tick(87000)).empty());
    b.Ready(90000);
    CHECK(Texts(b.Tick(95000)) == std::vector<std::string>{"Alpha disconnected"});
}
TEST_CASE("OotCoopRoom: re-announcing a held reconnect after our outage is silent", "[oot_coop]") {
    Room::PresenceBook b;
    b.Ready(0); b.Receive(Presence(), "Alpha", 0);
    b.Receive(Presence("peer-a", false), "Alpha", 10000);
    b.Receive(Presence(), "Alpha", 10001); // held reconnect before our own outage
    b.Lost(10002); b.Ready(11000);
    CHECK(Texts(b.Receive(Presence(), "Alpha", 11001)).empty());
    CHECK(Texts(b.Tick(20000)).empty());
}

TEST_CASE("OotCoopRoom: reconnect missing peers still obey the toast guard", "[oot_coop]") {
    Room::PresenceBook b;
    b.Ready(0); b.Receive(Presence(), "Alpha", 0); b.Lost(100); b.Ready(200);
    CHECK(Texts(b.Tick(5200)).empty());
    CHECK(Texts(b.Tick(10000)) == std::vector<std::string>{"Alpha disconnected"});
}
TEST_CASE("OotCoopRoom: completion is once per origin and local read is edge triggered", "[oot_coop]") {
    Room::PresenceBook b;
    b.Receive(Presence(), "Alpha", 0);
    const auto finish = b.Receive(Presence("peer-a", true, true), "Alpha", 1);
    REQUIRE(finish.size() == 1);
    CHECK(finish[0].id == "finish/peer-a");
    CHECK(finish[0].text == "Alpha finished the game");
    CHECK(Texts(b.Receive(Presence("peer-a", true, true), "Alpha", 2)).empty());
    b.Ready(0); b.Lost(3); b.Ready(4);
    CHECK(Texts(b.Receive(Presence("peer-a", true, true), "Alpha", 5)).empty());
    auto other = Texts(b.Receive(Presence("peer-b", true, true), "Beta", 6));
    CHECK(std::find(other.begin(), other.end(), "Beta finished the game") != other.end());
    Room::FinishWatch completed_save;
    CHECK_FALSE(completed_save.Sample(std::nullopt));
    CHECK_FALSE(completed_save.Sample(true));
    CHECK_FALSE(completed_save.Sample(true));
    completed_save.Rebaseline();
    CHECK_FALSE(completed_save.Sample(true));
    Room::FinishWatch watch;
    CHECK_FALSE(watch.Sample(false));
    CHECK_FALSE(watch.Sample(std::nullopt));
    CHECK(watch.Sample(true));
    CHECK_FALSE(watch.Sample(false));
    CHECK_FALSE(watch.Sample(true));
    watch.Rebaseline();
    CHECK_FALSE(watch.Sample(false));
    CHECK_FALSE(watch.Sample(true));
}
TEST_CASE("OotCoopRoom: strict finish and additive sidecar fields", "[oot_coop]") {
    CHECK(Room::DecodeFinish({{"bool", "0x0000002a"}}) == 42);
    for (const Json& bad : {Json{{"bool", "0x00000000"}}, Json{{"bool", "0x2a"}},
             Json{{"bool", "0x0000002g"}}, Json{{"bool", 42}}, Json{{"bool", "0x100000000"}},
             Json{{"bool", "0x0000002a"}, {"extra", 1}}, Json::array(), Json::object()})
        CHECK_FALSE(Room::DecodeFinish(bad));
    Json d{{"finish", {{"bool", "0x0000002a"}}}, {"area_names", {{"SyntheticZone", "Test area"}}},
           {"builds", {{"synthetic-build", {{"minimap", Minimap()}}}}}, {"unknown", true}};
    const auto c = Ux::DecodeConfig(d, "synthetic-build");
    REQUIRE(c.room);
    CHECK(c.room->finish == 42);
    CHECK(c.room->area_names.at("SyntheticZone") == "Test area");
    CHECK_FALSE(Room::DecodeConfig(d, "synthetic-build", false).minimap);
    CHECK(Room::DecodeConfig(d, "synthetic-build", true).minimap);
    CHECK_FALSE(Room::DecodeConfig(d, "other-build", true).minimap);
}
TEST_CASE("OotCoopRoom: snapshot ranges across batches exclude live entries", "[oot_coop]") {
    Room::SyncTally t;
    t.Snapshot(2, 5);
    t.Drained(1, Kind::Item, true, 0);
    t.Drained(2, Kind::Bool, true, 0);
    t.Drained(3, Kind::Item, false, 0);
    CHECK(t.Settle(1000, true).empty());
    t.Drained(4, Kind::PouchTotal, true, 1000);
    t.Drained(5, {}, false, 1000);
    t.Drained(6, Kind::Quest, true, 1000);
    CHECK(t.Settle(1001, false).empty());
    const auto r = t.Settle(1001, true);
    REQUIRE(r.size() == 1);
    CHECK(r[0].flags == 1);
    CHECK(r[0].items == 1);
    CHECK(r[0].entries == 4);
    CHECK(r[0].Text() == "Synced from team: 1 flag, 1 item");
    CHECK(t.Settle(20000, true).empty());
}
TEST_CASE("OotCoopRoom: all tally classes, zero and timeout", "[oot_coop]") {
    Room::SyncTally t;
    t.Snapshot(1, 12);
    const std::vector<Kind> kinds{Kind::Bool, Kind::Bit, Kind::Quest, Kind::Int, Kind::Enum,
        Kind::Item, Kind::PouchAdd, Kind::PouchTotal, Kind::Key, Kind::Rupee, Kind::Pose};
    for (size_t i = 0; i < kinds.size(); ++i) t.Drained(i+1, kinds[i], true, 1);
    t.Drained(12, {}, false, 10);
    t.Snapshot(13, 13); t.Drained(13, {}, false, 20);
    CHECK(t.Settle(15009, false).empty());
    auto r = t.Settle(15010, false);
    REQUIRE(r.size() == 1);
    CHECK(r[0].Text() == "Synced from team: 5 flags, 3 items");
    r = t.Settle(15020, false);
    REQUIRE(r.size() == 1);
    CHECK(r[0].Text().empty());
    CHECK(Room::SyncResult{0, 3, 0, 0}.Text() == "Synced from team: 3 flags");
    CHECK(Room::SyncResult{0, 0, 1, 0}.Text() == "Synced from team: 1 item");
    CHECK(Room::SyncResult{0, 0, 2, 0}.Text() == "Synced from team: 2 items");
}
TEST_CASE("OotCoopRoom: window formatting and area names", "[oot_coop]") {
    CHECK(Room::PrettyArea("FooBarBaz_Qux") == "Foo Bar Baz Qux");
    CHECK(Room::PrettyArea("ABCTest_Zone") == "ABC Test Zone");
    CHECK(Room::Seen(2999, 0) == "now");
    CHECK(Room::Seen(3000, 0) == "seen 3s");
    CHECK(Room::Seen(3999, 0) == "seen 3s");
    CHECK(Room::Seen(59000, 0) == "seen 59s");
    CHECK(Room::Seen(60000, 0) == "seen 1m");
    CHECK(Room::Seen(119999, 0) == "seen 1m");
    Room::WindowInfo i{"test", "abc", "flags-1234567890", "connected", "Local", 12,
        Room::RoomPeer{{}, {}, "SyntheticZone", true, true, true, false, 0}};
    Room::Config c;
    c.area_names["SyntheticZone"] = "Test area";
    std::vector<Room::RoomPeer> peers{
        {"b", "Beta", "FooBarBaz_Qux", true, true, false, false, 0},
        {"a", "Alpha", "SyntheticZone", false, true, true, false, 0},
        {"c", "Gamma", "", true, false, false, false, 6000}};
    auto lines = Room::WindowText(i, peers, c, 7000);
    REQUIRE(lines.size() == 6);
    CHECK(lines[0] == "Room test  seed 00000abc  mod 12345678");
    CHECK(lines[1] == "Relay connected  rtt 12 ms");
    CHECK(lines[2] == "You (Local)  child Test area");
    CHECK(lines[3] == "Beta         online  adult Foo Bar Baz Qux  seen 7s");
    CHECK(lines[4].starts_with("Gamma        online    -  "));
    CHECK(lines[4].ends_with("now"));
    CHECK(lines[5].starts_with("Alpha        offline child Test area"));
    peers[0].loading = true;
    CHECK(Room::WindowText(i, peers, c, 7000)[3].find("loading") != std::string::npos);
    for (int n = 0; n < 12; ++n) peers.push_back({std::to_string(n), "Z\n\xFF", "Bad\nArea", false, true});
    i.room = std::string(80, '\n'); i.name = "\xFF"; i.policy = "bad\npolicy";
    lines = Room::WindowText(i, peers, c, 7000);
    CHECK(lines.size() == 11);
    for (const auto& line : lines) {
        CHECK(line.size() <= 56);
        CHECK(std::all_of(line.begin(), line.end(), [](unsigned char ch) { return ch >= 32 && ch < 127; }));
    }
}
TEST_CASE("OotCoopRoom: marker geometry north-up, rotations, axes and clipping", "[oot_coop]") {
    auto m = *Room::DecodeMinimap(Minimap());
    auto p = Room::PlaceMarker(m, {50, 0, 50}, 90, {2, 0, 2});
    REQUIRE(p);
    CHECK(std::abs(p->x - 0.6f) < 1e-6f);
    CHECK(std::abs(p->y - 0.55f) < 1e-6f);
    CHECK_FALSE(p->clamped);
    m.player_centre = true;
    p = Room::PlaceMarker(m, {2, 0, 2}, 0, {2, 0, 2});
    REQUIRE(p); CHECK(p->x == 0.5f); CHECK(p->y == 0.5f);
    m.player_rotation = true;
    p = Room::PlaceMarker(m, {0, 0, 0}, 90, {2, 0, 0});
    REQUIRE(p); CHECK(std::abs(p->x - 0.5f) < 1e-6f); CHECK(std::abs(p->y - 0.55f) < 1e-6f);
    m.yaw_sign = -1;
    p = Room::PlaceMarker(m, {0, 0, 0}, 90, {2, 0, 0});
    REQUIRE(p); CHECK(std::abs(p->y - 0.45f) < 1e-6f);
    m.player_rotation = false;
    p = Room::PlaceMarker(m, {}, 0, {8, 0, 4});
    REQUIRE(p); CHECK(p->clamped); CHECK(std::abs(p->x - 0.7f) < 1e-6f);
    CHECK(std::abs(p->y - 0.55f) < 1e-6f);
    m.circle = true;
    p = Room::PlaceMarker(m, {}, 0, {8, 0, 8});
    REQUIRE(p); CHECK(p->clamped);
    CHECK(std::abs(std::hypot((p->x-0.5f)/0.2f, (p->y-0.5f)/0.1f) - 1) < 1e-5f);
    m.swap_xz = true; m.x_sign = -1; m.z_sign = -1;
    p = Room::PlaceMarker(m, {}, 0, {2, 0, 0});
    REQUIRE(p); CHECK(std::abs(p->y - 0.45f) < 1e-6f);
    CHECK_FALSE(Room::PlaceMarker(m, {}, std::numeric_limits<float>::quiet_NaN(), {}));
    CHECK_FALSE(Room::PlaceMarker(m, {}, 0, {0, 0, std::numeric_limits<float>::infinity()}));
    for (const float scale : {0.0f, -1.0f, std::numeric_limits<float>::infinity()}) {
        m.scale = scale;
        CHECK_FALSE(Room::PlaceMarker(m, {}, 0, {}));
    }
}
TEST_CASE("OotCoopRoom: minimap schema requires every field and no extras", "[oot_coop]") {
    const auto good = Minimap();
    REQUIRE(Room::DecodeMinimap(good));
    for (const auto& [key, value] : good.items()) {
        auto bad = good; bad.erase(key);
        CHECK_FALSE(Room::DecodeMinimap(bad));
    }
    const Json invalid{{"unknown", 1}, {"clip", "triangle"}, {"scale", 0},
        {"yaw_sign", 0}, {"half", {0, 1}}, {"swap_xz", 1}, {"centre", {2, 2}}, {"x_sign", true}};
    for (const auto& [key, value] : invalid.items()) {
        auto bad = good; bad[key] = value;
        CHECK_FALSE(Room::DecodeMinimap(bad));
    }
    int fake_memory = 0;
    CHECK_FALSE(Room::ReadMinimap(fake_memory, 0, *Room::DecodeMinimap(good)));
}
TEST_CASE("OotCoopRoom: panel cache and marker raster share screenshot layout", "[oot_coop]") {
    Overlay::PanelCache cache;
    Ux::Panel panel{{"Room synthetic", "You (Test)  child -"}, 1};
    const Overlay::Rect screen{100, 0, 1100, 800};
    const auto a = cache.Get(panel, 1, screen, 1200, 800);
    REQUIRE(a);
    CHECK(cache.Get(panel, 1, screen, 1200, 800) == a);
    CHECK(cache.Get(panel, 1, screen, 1200, 800)->rgba.data() == a->rgba.data());
    panel.lines[0] = "Room updated"; ++panel.generation;
    const auto b = cache.Get(panel, 1, screen, 1200, 800);
    REQUIRE(b);
    CHECK(a != b); CHECK(a->rgba != b->rgba); CHECK(a->cache_id != b->cache_id);
    CHECK(cache.Get(panel, 2, screen, 1200, 800) != b);
    CHECK(cache.Get(panel, 1, {0, 0, 1100, 800}, 1200, 800)->x == 16);
    Ux::OverlayFrame frame;
    frame.panel = std::make_shared<const Ux::Panel>(panel);
    frame.markers = {{{0.5f, 0.5f}, 0, 'A'}, {{0, 0}, 1, 'B'}};
    const auto boxes = Overlay::Layout(frame, screen, 1200, 800);
    REQUIRE(boxes.size() == 3);
    CHECK(boxes[0].x == 116);
    CHECK(boxes[1].width == boxes[1].height);
    CHECK(boxes[1].x + static_cast<s32>(boxes[1].width/2) == 600);
    CHECK(boxes[2].x == 100);
    CHECK(boxes[2].y == 0);
    std::vector<u8> pixels(1200 * 800 * 4);
    Overlay::BlendBgra(pixels, 1200, 800, boxes);
    CHECK(pixels[(20 * 1200 + 120) * 4] != 0);
    CHECK(pixels[(400 * 1200 + 600) * 4] != 0);
    Ux::Reset();
    Ux::PublishPanel(panel.lines); Ux::PublishMarkers(frame.markers);
    const auto first = Ux::Current();
    REQUIRE(first.panel);
    CHECK(first.markers.size() == 2);
    Ux::PublishPanel(panel.lines);
    CHECK(Ux::Current().panel == first.panel);
    Ux::Reset(); CHECK(Ux::Current().Empty());
    Ux::PublishPanel(panel.lines);
    CHECK(Ux::Current().panel->generation > first.panel->generation);
    Ux::Reset();
}
