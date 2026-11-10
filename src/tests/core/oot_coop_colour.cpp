// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstring>
#include <catch2/catch_test_macros.hpp>
#include "core/arm/dynarmic/oot_coop_colour.h"
#include "core/arm/dynarmic/oot_coop_sync.h"

namespace {
namespace C = Core::OotCoop::Colour;
namespace E = Core::OotCoop::Equip;
namespace T = Core::OotGameDataTrace;
using Json = nlohmann::json;
Json Entry() { return {{"at", "0x400"}, {"words", {1, 2, 3, 4, 5, 6, 7, 8}}}; }
Json Keys() {
    return {{"enabled", true}, {"apply", Entry()}, {"animation", "TestPalette"},
        {"frames", {11, 12, 13, 14}}, {"players", {{"Peer A", 21}, {"Stock", 0}}}};
}
Json Precheck() {
    auto j = Keys();
    j["instance"] = {48, 16, 24, 40}; j["getter"] = Entry();
    j["bank"] = {16, 24, 32, 8, 16}; j["record_stride"] = 24; j["record_name"] = 8;
    return j;
}
T::GhostOffsets Ghost() {
    T::GhostOffsets g;
    g.link = {8, 16, 8, 16}; g.actor_fields = {24};
    g.lifecycle = 32; g.lifecycle_lo = 5; g.lifecycle_hi = 9;
    return g;
}
struct Rig {
    std::shared_ptr<const C::Keys> keys;
    C::Controller controller;
    std::vector<u8> bytes = std::vector<u8>(0x10000);
    std::vector<C::View> views;
    std::vector<std::string> logs;
    std::vector<std::array<u64, 8>> calls;
    std::vector<float> frames;
    C::Port port;
    std::string name, scene{"TestScene"};
    bool ready{true}, call_ok{true}, verified{true}, write_ok{true}, invalidate_write{};
    u64 tick{1};
    template <typename V> void Put(u64 at, V value) { std::memcpy(bytes.data() + at, &value, sizeof(value)); }
    template <typename V> std::optional<V> Get(u64 at) {
        if (!at || at > bytes.size() || sizeof(V) > bytes.size() - at) return {};
        V v{}; std::memcpy(&v, bytes.data() + at, sizeof(v)); return v;
    }
    explicit Rig(Json json = Keys()) : keys{C::DecodeKeys(json)}, controller{keys, Ghost()} {
        port = {
            [this](u64 at) { return Get<u8>(at); }, [this](u64 at) { return Get<u32>(at); },
            [this](u64 at) { return Get<u64>(at); },
            [this](u64 at, const void* data, size_t n) {
                if (!write_ok || at > bytes.size() || n > bytes.size() - at) return false;
                std::memcpy(bytes.data() + at, data, n);
                if (invalidate_write) Put<u64>(views[0].actor, 0);
                return true;
            },
            [this](const T::Entry& e) -> std::optional<u64> { return verified ? std::optional{e.offset} : std::nullopt; },
            [this](u64, const std::array<u64, 8>& args, std::optional<float> frame) {
                if (!call_ok) return false;
                calls.push_back(args); frames.push_back(frame.value_or(-1)); return true;
            }, [this](std::string_view line) { logs.emplace_back(line); }};
        C::View v;
        v.origin = "peer0001"; v.banc = scene; v.actor = 0x1000; v.record = 0x2000;
        v.vtable = 0x3000; v.session = 1; v.epoch = 1; v.generation = 7; v.fields = {0x4000};
        v.live = v.placed = v.alpha_done = true;
        views.push_back(v); Install(v);
    }
    void Install(const C::View& v) {
        Put(v.actor, v.vtable); Put(v.actor + 8, v.record); Put(v.record + 8, v.actor);
        Put(v.record + 16, v.generation); Put(v.actor + 24, v.fields[0]); Put<u32>(v.actor + 32, 1);
    }
    bool Frame() {
        return controller.Frame(port, views, scene, ready,
            [this](std::string_view) { return name; }, tick, 0x8000);
    }
    void Return() {
        controller.Returned(port, views, scene, ready, [this](std::string_view) { return name; });
    }
    void Bank(bool present) {
        Put<u64>(0x1030, 0x5000); Put<u64>(0x5010, 0x5100); Put<u64>(0x5100, 0x5200);
        Put<u64>(0x5218, 0x400); Put<u64>(0x5128, 0x5300); Put<u64>(0x5310, 0x5400);
        Put<u32>(0x5418, 1); Put<u64>(0x5420, 0x5500); Put<u64>(0x5500, 0x5600);
        Put<u16>(0x5608, 1); Put<u16>(0x560a, 0xbeef); Put<u64>(0x5610, 0x5700); Put<u64>(0x5708, 0x5800);
        const std::string text = present ? keys->animation : "OtherPalette";
        std::memcpy(bytes.data() + 0x5800, text.c_str(), text.size() + 1);
    }
};
}

TEST_CASE("Colour optional profile and strict keys", "[core][oot_coop]") {
    const std::string build(40, 'A');
    Json document{{"offsets", {{build, {{"manager_cell", 256}}}}}};
    auto profile = Core::OotCoop::DecodeProfile(document, build);
    REQUIRE(profile); REQUIRE_FALSE(profile->colour);
    document["offsets"][build]["colour"] = Keys();
    profile = Core::OotCoop::DecodeProfile(document, build);
    REQUIRE(profile); REQUIRE(profile->colour);
    REQUIRE(C::DecodeKeys(Keys())); REQUIRE(C::DecodeKeys(Precheck()));
    REQUIRE(C::DecodeKeys(Json{{"enabled", false}}));
    REQUIRE_FALSE(C::DecodeKeys(Json::object())); REQUIRE_FALSE(C::DecodeKeys(nullptr));
    auto k = C::DecodeKeys(Keys()); REQUIRE(k->hide_armour);
    auto off = Keys(); off["enabled"] = false; REQUIRE(C::DecodeKeys(off));
    for (const auto& field : {"enabled", "apply", "animation", "frames", "players", "hide_armour",
                              "instance", "getter", "bank", "record_stride", "record_name"}) {
        auto j = Precheck(); j[field] = nullptr;
        INFO(field); REQUIRE_FALSE(C::DecodeKeys(j));
        j[field] = Json::array(); REQUIRE_FALSE(C::DecodeKeys(j));
    }
    for (const auto& field : {"apply", "getter"}) {
        auto j = Precheck(); j[field]["unknown"] = 1; REQUIRE_FALSE(C::DecodeKeys(j));
        for (const auto n : {0, 7, 9, 17}) {
            j = Precheck(); j[field]["words"] = std::vector<int>(n, 1); REQUIRE_FALSE(C::DecodeKeys(j));
        }
        for (const Json& bad : {Json(0), Json(-4), Json(3), Json(0x10000004), Json(true), Json(4.5), Json("0xZ")}) {
            j = Precheck(); j[field]["at"] = bad; REQUIRE_FALSE(C::DecodeKeys(j));
        }
        for (const Json& bad : {Json(-1), Json(0x100000000ULL), Json(true), Json(1.5), Json("bad")}) {
            j = Precheck(); j[field]["words"][0] = bad; REQUIRE_FALSE(C::DecodeKeys(j));
        }
    }
    auto j = Keys(); j["unknown"] = false; REQUIRE_FALSE(C::DecodeKeys(j));
    for (const auto& text : {std::string{}, std::string(24, 'a'), std::string("a\nb"), std::string("\x80")}) {
        j = Keys(); j["animation"] = text; REQUIRE_FALSE(C::DecodeKeys(j));
    }
    j = Keys(); j["animation"] = std::string(23, 'a'); REQUIRE(C::DecodeKeys(j));
    for (const Json& bad : {Json(0), Json(-1), Json(256), Json(true), Json(1.5), Json("1"), Json(12)}) {
        j = Keys(); j["frames"][0] = bad; REQUIRE_FALSE(C::DecodeKeys(j));
    }
    for (const auto n : {0, 3, 17}) {
        j = Keys(); j["frames"] = std::vector<int>(n, 1); REQUIRE_FALSE(C::DecodeKeys(j));
    }
    for (const auto& name : {std::string{}, std::string(33, 'a'), std::string("bad\0name", 8), std::string("\xFF")}) {
        j = Keys(); j["players"] = Json::object(); j["players"][name] = 1; REQUIRE_FALSE(C::DecodeKeys(j));
    }
    j = Keys(); j["players"] = {{"\xc3\xa9", 255}, {std::string(32, 'a'), 0}}; REQUIRE(C::DecodeKeys(j));
    for (const Json& bad : {Json(-1), Json(256), Json(true), Json(1.5), Json("1"), Json::object()}) {
        j = Keys(); j["players"]["Peer"] = bad; REQUIRE_FALSE(C::DecodeKeys(j));
    }
    for (const auto& field : {"instance", "bank"}) {
        j = Precheck(); j[field].erase(j[field].begin()); REQUIRE_FALSE(C::DecodeKeys(j));
        for (const Json& bad : {Json(-1), Json(65537), Json(true), Json(1.5)}) {
            j = Precheck(); j[field][0] = bad; REQUIRE_FALSE(C::DecodeKeys(j));
        }
    }
    j = Precheck(); j["record_stride"] = 0; REQUIRE_FALSE(C::DecodeKeys(j));
    j = Precheck(); j["record_name"] = 24; REQUIRE_FALSE(C::DecodeKeys(j));
    j = Precheck(); j.erase("getter"); REQUIRE_FALSE(C::DecodeKeys(j));
    j = Precheck(); j.erase("instance"); REQUIRE_FALSE(C::DecodeKeys(j));
    j = Keys(); j["enabled"] = false; j["apply"]["oops"] = 1; REQUIRE_FALSE(C::DecodeKeys(j));
}
TEST_CASE("Colour desired frame and equipment masking", "[core][oot_coop]") {
    const auto k = C::DecodeKeys(Keys());
    REQUIRE(C::DesiredFrame(*k, 2, "Other") == 13);
    REQUIRE(C::DesiredFrame(*k, 2, "Peer A") == 21);
    REQUIRE(C::DesiredFrame(*k, 2, "Stock") == 0);
    REQUIRE_FALSE(C::MaskEquipment({}, false, true));
    for (u8 tunic = 0; tunic < 4; ++tunic) {
        E::Appearance appearance{1, 2, 1, 2, tunic, 2, 4, 1, 1};
        const E::Packet packet{*E::Pack(appearance), 4321};
        auto masked = C::MaskEquipment(packet, false, true);
        REQUIRE(masked); REQUIRE(masked->sequence == packet.sequence);
        appearance.tunic = 0;
        REQUIRE(E::Unpack(masked->word) == appearance);
        REQUIRE(C::MaskEquipment(packet, false, false) == packet);
        appearance.tunic = tunic; appearance.age = 0;
        const E::Packet child{*E::Pack(appearance), 98};
        REQUIRE(C::MaskEquipment(child, true, true) == child);
        REQUIRE(C::MaskEquipment(child, false, true) == child); // invalid adult age
    }
    const E::Packet invalid{0xffffffff, 77};
    REQUIRE(C::MaskEquipment(invalid, false, true) == invalid);
    auto j = Keys(); j["hide_armour"] = false;
    Rig r{j}; REQUIRE_FALSE(r.controller.On(0, "")); REQUIRE(r.Frame());
}
TEST_CASE("Colour issues once and writes its own cell and name", "[core][oot_coop]") {
    Rig r; REQUIRE(r.Frame()); REQUIRE(r.controller.Calling()); REQUIRE_FALSE(r.Frame());
    REQUIRE(r.calls.size() == 1); REQUIRE(r.calls[0][0] == r.views[0].actor);
    REQUIRE(r.calls[0][1] == 0x8000 + C::CellAt); REQUIRE(r.frames[0] == 11.0f);
    REQUIRE(r.Get<u64>(0x8000 + C::CellAt) == 0x8000 + C::CellAt + 8);
    REQUIRE(std::memcmp(r.bytes.data() + 0x8000 + C::CellAt + 8,
                        r.keys->animation.c_str(), r.keys->animation.size() + 1) == 0);
    REQUIRE(r.bytes[0x8000 + C::CellAt - 1] == 0); REQUIRE(r.bytes[0x8000 + C::CellAt + C::CellBytes] == 0);
    r.Return(); REQUIRE_FALSE(r.controller.Calling());
    r.tick += 60; REQUIRE_FALSE(r.Frame());
    ++r.views[0].generation; r.Install(r.views[0]); REQUIRE(r.Frame()); r.Return();
    ++r.tick; r.name = "Peer A"; REQUIRE(r.Frame()); REQUIRE(r.frames.back() == 21.0f); r.Return();
    ++r.tick; r.name = "Stock"; REQUIRE_FALSE(r.Frame()); REQUIRE_FALSE(r.controller.On(0, r.name));
    ++r.tick; r.name.clear(); ++r.views[0].epoch; REQUIRE(r.Frame()); r.Return();
    ++r.tick; r.views[0].actor = 0x1800; r.Install(r.views[0]); REQUIRE(r.Frame());
}
TEST_CASE("Colour gates readiness and all ghost lifetime states", "[core][oot_coop]") {
    for (int gate = 0; gate < 10; ++gate) {
        Rig r;
        switch (gate) {
        case 0: r.views[0].parked = true; break;
        case 1: r.views[0].placed = false; break;
        case 2: r.views[0].abandon = true; break;
        case 3: r.views[0].live = false; break;
        case 4: r.ready = false; break;
        case 5: r.scene = "OtherScene"; break;
        case 6: r.views[0].alpha_done = false; break;
        case 7: r.name = "Stock"; break;
        case 8: r.views[0].actor = 0; break;
        case 9: r.scene.clear(); break;
        }
        INFO(gate); REQUIRE_FALSE(r.Frame()); REQUIRE(r.calls.empty());
    }
    Rig disabled{Json{{"enabled", false}}}; REQUIRE_FALSE(disabled.Frame());
}
TEST_CASE("Colour revalidates every actor field before issue and return", "[core][oot_coop]") {
    for (int mutation = 0; mutation < 8; ++mutation) {
        for (bool returning : {false, true}) {
            Rig r;
            if (returning) REQUIRE(r.Frame());
            switch (mutation) {
            case 0: r.Put<u64>(0x1008, 0); break;
            case 1: r.Put<u64>(0x2008, 0); break;
            case 2: r.Put<u32>(0x2010, 99); break;
            case 3: r.Put<u64>(0x1000, 0); break;
            case 4: r.Put<u64>(0x1018, 0); break;
            case 5: r.Put<u32>(0x1020, 5); break;
            case 6: r.views[0].fields.clear(); break;
            case 7: r.views[0].record = ~u64{0}; break;
            }
            INFO(mutation); INFO(returning);
            if (returning) { r.Return(); REQUIRE(r.logs.back().find("drop on return") != std::string::npos); }
            else { REQUIRE_FALSE(r.Frame()); REQUIRE(r.calls.empty()); }
        }
    }
    Rig r; r.invalidate_write = true; REQUIRE_FALSE(r.Frame()); REQUIRE(r.calls.empty());
}
TEST_CASE("Colour dropped returns retry with a bounded spacing and cap", "[core][oot_coop]") {
    Rig r;
    for (int attempt = 0; attempt < 3; ++attempt) {
        REQUIRE(r.Frame()); r.ready = false; r.Return(); r.ready = true;
        r.tick += 29; REQUIRE_FALSE(r.Frame()); ++r.tick;
    }
    REQUIRE_FALSE(r.Frame()); REQUIRE(r.calls.size() == 3);
    const auto count = r.logs.size(); r.tick += 100; REQUIRE_FALSE(r.Frame()); REQUIRE(r.logs.size() == count);
    ++r.views[0].generation; r.Install(r.views[0]); REQUIRE(r.Frame());
    Rig retry; REQUIRE(retry.Frame()); retry.Put<u64>(0x1000, 0); retry.Return();
    retry.Install(retry.views[0]); retry.tick += 30; REQUIRE(retry.Frame()); retry.Return();
    retry.tick += 30; REQUIRE_FALSE(retry.Frame());
    for (int gate = 0; gate < 3; ++gate) {
        Rig failed;
        if (gate == 0) failed.verified = false;
        if (gate == 1) failed.write_ok = false;
        if (gate == 2) failed.call_ok = false;
        for (int n = 0; n < 5; ++n) { REQUIRE_FALSE(failed.Frame()); failed.tick += 30; }
        REQUIRE(failed.calls.empty()); REQUIRE(failed.logs.back().find("give-up") != std::string::npos);
    }
}
TEST_CASE("Colour cannot accept changed identity or selection on return", "[core][oot_coop]") {
    for (int mutation = 0; mutation < 7; ++mutation) {
        Rig r; REQUIRE(r.Frame());
        switch (mutation) {
        case 0: ++r.views[0].generation; break;
        case 1: ++r.views[0].session; break;
        case 2: ++r.views[0].epoch; break;
        case 3: r.views[0].child = true; break;
        case 4: r.name = "Peer A"; break;
        case 5: r.views[0].abandon = true; break;
        case 6: r.scene = "OtherScene"; break;
        }
        r.Return(); REQUIRE(r.logs.back().find("drop on return") != std::string::npos);
    }
}
TEST_CASE("Colour fairly schedules four slots with one call per frame", "[core][oot_coop]") {
    Rig r;
    for (size_t i = 1; i < 4; ++i) {
        auto v = r.views[0]; v.origin += std::to_string(i); v.actor += i * 0x100; v.record += i * 0x100;
        r.views.push_back(v); r.Install(v);
    }
    for (size_t i = 0; i < 4; ++i) {
        REQUIRE(r.Frame()); REQUIRE(r.calls.back()[0] == r.views[i].actor);
        REQUIRE(r.calls.back()[1] == 0x8000 + i * 0x200 + C::CellAt);
        r.Return(); REQUIRE_FALSE(r.Frame()); ++r.tick;
    }
    REQUIRE_FALSE(r.Frame()); REQUIRE(r.calls.size() == 4);
}
TEST_CASE("Colour precheck separates missing unreadable and present banks", "[core][oot_coop]") {
    Rig present{Precheck()}; present.Bank(true); REQUIRE(present.Frame());
    REQUIRE(present.logs.front().find("palette present banks=1") != std::string::npos);
    Rig missing{Precheck()}; missing.Bank(false); REQUIRE_FALSE(missing.Frame()); REQUIRE(missing.calls.empty());
    REQUIRE(missing.logs.front().find("palette missing banks=1") != std::string::npos);
    missing.Bank(true); missing.tick += 30; REQUIRE(missing.Frame());
    REQUIRE(std::count_if(missing.logs.begin(), missing.logs.end(), [](const auto& s) {
        return s.find("banks=") != std::string::npos; }) == 1);
    for (int corruption = 0; corruption < 5; ++corruption) {
        Rig r{Precheck()}; r.Bank(true);
        switch (corruption) {
        case 0: r.Put<u64>(0x5218, 0); break;
        case 1: r.Put<u32>(0x5418, 65); break;
        case 2: r.Put<u32>(0x5608, 257); break;
        case 3: r.Put<u64>(0x5708, ~u64{0}); break;
        case 4: r.Put<u64>(0x5420, 0); break;
        }
        REQUIRE_FALSE(r.Frame()); REQUIRE(r.calls.empty());
    }
}
