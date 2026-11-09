// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/arm/dynarmic/oot_coop_ux.h"
#include "video_core/coop_overlay.h"

// Synthetic offsets, hashes and names only: nothing here is a value from the game.
namespace {
namespace Ux = Core::OotCoop::Ux;
using Json = nlohmann::json;

struct FakeMemory {
    std::map<u64, u8> bytes;
    bool IsValidVirtualAddressRange(u64 address, u64 size) const {
        for (u64 i = 0; i < size; ++i)
            if (!bytes.contains(address + i)) return false;
        return true;
    }
    u32 Read32(u64 address) const {
        u32 v = 0;
        for (int i = 3; i >= 0; --i) v = (v << 8) | bytes.at(address + i);
        return v;
    }
    u64 Read64(u64 address) const { return Read32(address) | (u64{Read32(address + 4)} << 32); }
    void W32(u64 address, u32 v) {
        for (int i = 0; i < 4; ++i) bytes[address + i] = static_cast<u8>(v >> (8 * i));
    }
    void W64(u64 address, u64 v) {
        W32(address, static_cast<u32>(v));
        W32(address + 4, static_cast<u32>(v >> 32));
    }
    void WF(u64 address, float f) {
        u32 v;
        std::memcpy(&v, &f, 4);
        W32(address, v);
    }
};

Json CameraJson() {
    return {{"registry_cell", "0x100"}, {"registry_table", "0x10"}, {"mask", "0x20"},
            {"buckets", "0x30"}, {"bucket_capacity", 0}, {"bucket_hashes", 8}, {"bucket_count", 16},
            {"bucket_index", 24}, {"main_hash", "0xABCD1234"}, {"assoc_cell", "0x200"},
            {"assoc_slot", "0x10"}, {"assoc_slot_stride", 8}, {"assoc_entry", "0x100"},
            {"assoc_entry_stride", 16}, {"projection", 8}, {"views_cell", "0x300"},
            {"views_count", 0}, {"views_array", 8}, {"view_flags", "0x12"}, {"view_special", "0x20"},
            {"view_special_offset", 4}, {"view_pointer", "0x30"}, {"special_any", 3},
            {"special_all", 2}, {"view_matrix", 4}, {"projection_dirty", 4},
            {"projection_matrix", 8}, {"head_child", 1.0}, {"head_adult", 1.5}};
}

// An identity view (camera at the origin looking down -z) and a simple perspective.
Ux::Camera Simple() {
    Ux::Camera c;
    c.view = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    // clip.x = x, clip.y = y, clip.w = -z
    c.projection = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0, 0, 0, -1, 0};
    return c;
}

// The synthetic guest layout CameraJson() describes, holding `camera`.
FakeMemory CameraMemory(u64 base, const Ux::Camera& camera, bool special) {
    FakeMemory m;
    const u64 S = 0x10000, T = 0x11000, B = 0x12000, H = 0x13000, R = 0x14000, A = 0x15000,
              P = 0x16000, D = 0x17000, Arr = 0x18000, L = 0x19000, V = 0x1A000, X = 0x1B000;
    m.W64(base + 0x100, S);
    m.W64(S + 0x10, T);
    m.W32(T + 0x20, 0b100); // bucket 2 only
    m.W64(T + 0x30 + 8 * 2, B);
    m.W32(B + 0, 4);
    m.W64(B + 8, H);
    m.W32(B + 16, 3);
    m.W32(B + 24, 5); // render index 5
    m.W32(H, 0x11111111);
    m.W32(H + 4, 0xABCD1234);
    m.W32(H + 8, 0xFFFFFFFF);
    m.W64(base + 0x200, R);
    m.W32(R + 0x10 + 8 * 5, 7); // slot 7
    m.W64(R + 0x100 + 16 * 7, A);
    m.W64(A + 8, P);
    m.W64(base + 0x300, D);
    m.W32(D, 8);
    m.W64(D + 8, Arr);
    m.W64(Arr + 8 * 5, L);
    m.W32(L + 0x12, special ? 3 : 0);
    m.W64(L + 0x20, X);
    m.W64(L + 0x30, V);
    const u64 view = special ? X + 4 : V;
    for (int i = 0; i < 12; ++i) m.WF(view + 4 + 4 * i, camera.view[i]);
    for (int i = 0; i < 12; ++i) m.WF((special ? V : X + 4) + 4 + 4 * i, 99.0f); // the other one
    m.W32(P + 4, 0);
    for (int i = 0; i < 16; ++i) m.WF(P + 8 + 4 * i, camera.projection[i]);
    return m;
}
} // namespace

TEST_CASE("OotCoopUx: switches and names", "[oot_coop]") {
    ::setenv("EDEN_NX2_OOT_COOP_UXTEST", "0", 1);
    CHECK_FALSE(Ux::Switch("UXTEST"));
    ::setenv("EDEN_NX2_OOT_COOP_UXTEST", "1", 1);
    CHECK(Ux::Switch("UXTEST", false));
    ::unsetenv("EDEN_NX2_OOT_COOP_UXTEST");
    CHECK(Ux::Switch("UXTEST"));
    CHECK_FALSE(Ux::Switch("UXTEST", false));
    CHECK(Ux::CleanName("Anthony") == "Anthony");
    CHECK(Ux::CleanName("") == "Player");
    CHECK(Ux::CleanName("  ") == "Player");
    CHECK(Ux::CleanName("D\xC3\xA9nis\n") == "D?nis?");
    CHECK(Ux::CleanName(std::string(40, 'x')).size() == 24);
    CHECK(Ux::CleanName("{}%s") == "{}%s"); // literal, never a format string
}

TEST_CASE("OotCoopUx: item and rupee text", "[oot_coop]") {
    Ux::ItemNames names;
    CHECK(names.Load({{"item_names", {{"Item_A", "Thing A"}, {"bad name", "x"}, {"Item_B", 3},
                                      {"Item_C", ""}}}}) == 1);
    CHECK(Ux::ItemText("Anthony", "Item_A", 1, names) == "Anthony got 1x Thing A");
    CHECK(Ux::ItemText("Anthony", "Item_A", 5, names) == "Anthony got 5x Thing A");
    CHECK(Ux::ItemText("Anthony", "Item_Unknown", 1, names) == "Anthony got an item");
    CHECK(Ux::ItemText("Anthony", "Item_Unknown", 3, names) == "Anthony got 3 items");
    CHECK(Ux::ItemText("", "Item_A", 1, names, true) == "You shared 1x Thing A");
    CHECK(Ux::RupeeText("Anthony", 20) == "Anthony got 20 rupees");
    CHECK(Ux::RupeeText("Anthony", 1) == "Anthony got 1 rupee");
    CHECK(Ux::RupeeText("Anthony", -5) == "Anthony spent 5 rupees");
}

TEST_CASE("OotCoopUx: toast queue dedup, order, expiry, reset", "[oot_coop]") {
    Ux::ToastQueue q;
    CHECK(q.Push("a/1", "one", 0));
    CHECK_FALSE(q.Push("a/1", "one again", 0)); // the same grant id: one toast
    CHECK_FALSE(q.Push("a/9", "", 0));
    CHECK(q.Push("a/2", "two", 0));
    CHECK(q.Push("b/1", "three", 0));
    CHECK(q.Push("b/2", "four", 0));
    CHECK(q.Visible(0) == std::vector<std::string>{"one", "two", "three"});
    CHECK(q.Visible(3999) == std::vector<std::string>{"one", "two", "three"});
    CHECK(q.Visible(4000) == std::vector<std::string>{"four"});
    CHECK(q.Visible(7999) == std::vector<std::string>{"four"});
    CHECK(q.Visible(8000).empty());
    CHECK(q.Empty());
    CHECK_FALSE(q.Push("a/1", "one", 9000)); // still deduplicated after it expired
    q.Clear();
    CHECK(q.Push("a/1", "one", 9000)); // a new session starts clean
    for (int i = 0; i < 40; ++i) q.Push("x/" + std::to_string(i), "t", 9000);
    CHECK(q.Visible(9000).size() == Ux::ToastQueue::MaxVisible);
}

TEST_CASE("OotCoopUx: camera keys decode strictly", "[oot_coop]") {
    const auto keys = Ux::DecodeCamera(CameraJson());
    REQUIRE(keys);
    CHECK(keys->main_hash == 0xABCD1234);
    CHECK(keys->registry_cell == 0x100);
    CHECK(keys->head_adult == 1.5f);
    auto missing = CameraJson();
    missing.erase("view_matrix");
    CHECK_FALSE(Ux::DecodeCamera(missing));
    auto unknown = CameraJson();
    unknown["surprise"] = 1;
    CHECK_FALSE(Ux::DecodeCamera(unknown));
    auto bad = CameraJson();
    bad["mask"] = "0xZZ";
    CHECK_FALSE(Ux::DecodeCamera(bad));
    auto tall = CameraJson();
    tall["head_child"] = 50.0;
    CHECK_FALSE(Ux::DecodeCamera(tall));
}

TEST_CASE("OotCoopUx: projection", "[oot_coop]") {
    const auto c = Simple();
    const auto centre = Ux::Project(c, {0, 0, -5}, false);
    REQUIRE(centre);
    CHECK(centre->x == 0.5f);
    CHECK(centre->y == 0.5f);
    const auto up = Ux::Project(c, {0, 2.5f, -5}, false);
    REQUIRE(up);
    CHECK(up->y == 0.25f); // +y up in NDC: higher on screen
    const auto flipped = Ux::Project(c, {0, 2.5f, -5}, true);
    REQUIRE(flipped);
    CHECK(flipped->y == 0.75f);
    const auto right = Ux::Project(c, {2.5f, 0, -5}, false);
    REQUIRE(right);
    CHECK(right->x == 0.75f);
    CHECK_FALSE(Ux::Project(c, {0, 0, 5}, false));   // behind the camera
    CHECK_FALSE(Ux::Project(c, {0, 0, 0}, false));   // at the eye
    CHECK_FALSE(Ux::Project(c, {20, 0, -5}, false)); // off screen
    auto broken = c;
    broken.projection[15] = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(Ux::Project(broken, {0, 0, -5}, false));
}

TEST_CASE("OotCoopUx: camera read chain", "[oot_coop]") {
    const auto keys = *Ux::DecodeCamera(CameraJson());
    const u64 base = 0x1000;
    auto camera = Simple();
    camera.view[3] = 7.0f;
    for (const bool special : {false, true}) {
        auto memory = CameraMemory(base, camera, special);
        Ux::CameraFail fail{};
        const auto read = Ux::ReadCamera(memory, base, keys, &fail);
        REQUIRE(read);
        CHECK(fail == Ux::CameraFail::None);
        CHECK(*read == camera);
    }
    auto memory = CameraMemory(base, camera, false);
    Ux::CameraFail fail{};
    memory.W32(0x16000 + 4, 1); // CPU projection dirty: skipped, never computed
    CHECK_FALSE(Ux::ReadCamera(memory, base, keys, &fail));
    CHECK(fail == Ux::CameraFail::Dirty);
    memory = CameraMemory(base, camera, false);
    memory.W32(0x13000 + 4, 0x22222222); // no Main view in the registry
    CHECK_FALSE(Ux::ReadCamera(memory, base, keys, &fail));
    CHECK(fail == Ux::CameraFail::NoMain);
    memory = CameraMemory(base, camera, false);
    memory.W32(0x12000 + 16, 9); // count above capacity
    CHECK_FALSE(Ux::ReadCamera(memory, base, keys, &fail));
    CHECK(fail == Ux::CameraFail::Registry);
    memory = CameraMemory(base, camera, false);
    memory.W32(0x14000 + 0x10 + 8 * 5, 40); // slot out of range
    CHECK_FALSE(Ux::ReadCamera(memory, base, keys, &fail));
    CHECK(fail == Ux::CameraFail::Slot);
    memory = CameraMemory(base, camera, false);
    memory.W32(0x17000, 5); // index 5 not below the view count
    CHECK_FALSE(Ux::ReadCamera(memory, base, keys, &fail));
    CHECK(fail == Ux::CameraFail::View);
    FakeMemory empty;
    CHECK_FALSE(Ux::ReadCamera(empty, base, keys, &fail));
    CHECK(fail == Ux::CameraFail::Root);
}

TEST_CASE("OotCoopUx: tags only for peers in our settled scene", "[oot_coop]") {
    const auto keys = *Ux::DecodeCamera(CameraJson());
    const auto c = Simple();
    std::vector<Ux::TagPeer> peers{{"Anthony", "SceneA", false, false, {0, -1.5f, -5}},
                                   {"Elsewhere", "SceneB", false, false, {0, 0, -5}},
                                   {"Loading", "SceneA", true, false, {0, 0, -5}},
                                   {"Behind", "SceneA", false, true, {0, 0, 5}}};
    auto tags = Ux::BuildTags(c, keys, "SceneA", false, peers, false);
    REQUIRE(tags.size() == 1);
    CHECK(tags[0].text == "Anthony");
    CHECK(std::abs(tags[0].at.y - 0.5f) < 1e-5f); // root -1.5 plus the adult height 1.5
    CHECK(Ux::BuildTags(c, keys, "SceneA", true, peers, false).empty());
    CHECK(Ux::BuildTags(c, keys, "", false, peers, false).empty());
    tags = Ux::BuildTags(c, keys, "SceneA", false, peers, true);
    REQUIRE(tags.size() == 3);
    CHECK(tags[0].marker);
    CHECK(std::abs(tags[0].at.y - 0.65f) < 1e-5f);
    CHECK(tags[1].marker);
    CHECK_FALSE(tags[2].marker);
}

TEST_CASE("OotCoopUx: sidecar config", "[oot_coop]") {
    const std::string build = "0123456789ABCDEF0123456789ABCDEF01234567";
    Json doc{{"item_names", {{"Item_A", "Thing A"}}}, {"builds", {{build, {{"camera", CameraJson()}}}}}};
    std::string why;
    auto config = Ux::DecodeConfig(doc, build, &why);
    CHECK(config.names.Size() == 1);
    CHECK(config.camera);
    config = Ux::DecodeConfig(doc, "FFFF456789ABCDEF0123456789ABCDEF01234567", &why);
    CHECK_FALSE(config.camera);
    CHECK(config.names.Size() == 1);
    CHECK(why.find("no camera keys") != std::string::npos);
}

TEST_CASE("OotCoopUx: session toasts, names, reset", "[oot_coop]") {
    Ux::Reset();
    Ux::SessionUx session;
    Json doc{{"item_names", {{"Item_Sword", "Sword"}}}};
    session.Configure(std::make_shared<const Ux::Config>(Ux::DecodeConfig(doc, "x")));
    CHECK(session.Name("peer-a") == "Player");
    session.SetName("peer-a", "Anthony");
    session.ItemReceived("peer-a", 4, "Item_Sword", 1);
    session.ItemReceived("peer-a", 4, "Item_Sword", 1); // the same grant: one toast
    session.RupeeReceived("peer-a", 5, 20);
    auto frame = Ux::Current();
    CHECK(frame.toasts == std::vector<std::string>{"Anthony got 1x Sword", "Anthony got 20 rupees"});
    Ux::PublishTags({{"Anthony", {0.5f, 0.5f}, false}});
    CHECK(Ux::Current().tags.size() == 1);
    Ux::Reset();
    frame = Ux::Current();
    CHECK(frame.Empty());
}

TEST_CASE("OotCoopUx: overlay boxes", "[oot_coop]") {
    namespace Overlay = VideoCore::CoopOverlay;
    CHECK(Overlay::Scale(720) == 1);
    CHECK(Overlay::Scale(1080) == 1);
    CHECK(Overlay::Scale(2160) == 2);
    Ux::OverlayFrame frame;
    frame.toasts = {"Anthony got 1x Kokiri Sword", "second"};
    frame.tags = {{"Anthony", {0.5f, 0.5f}, false}, {"", {0.25f, 0.25f}, true}};
    // A letterboxed game rectangle: x 240..1680 of a 1920 x 1080 frame.
    const Overlay::Rect screen{240, 0, 1680, 1080};
    const auto boxes = Overlay::Layout(frame, screen, 1920, 1080);
    REQUIRE(boxes.size() == 5); // tag, two cross bars, two toasts
    const auto& tag = boxes[0];
    CHECK(tag.x + static_cast<s32>(tag.width / 2) == 960);
    CHECK(tag.y + static_cast<s32>(tag.height) <= 540);
    const auto& toast = boxes[3];
    CHECK(toast.x + static_cast<s32>(toast.width) == 1680 - 16); // upper right of the game rectangle
    CHECK(toast.y == 16);
    CHECK(boxes[4].y > toast.y + static_cast<s32>(toast.height));
    for (const auto& box : boxes) {
        CHECK(box.rgba.size() == size_t{box.width} * box.height * 4);
        CHECK(box.x >= 240);
        CHECK(box.x + static_cast<s32>(box.width) <= 1680);
    }
    // Clipped at the edge: a tag near the left border stays inside the game rectangle.
    Ux::OverlayFrame edge;
    edge.tags = {{"A long peer name", {0.0f, 0.5f}, false}};
    const auto clipped = Overlay::Layout(edge, screen, 1920, 1080);
    REQUIRE(clipped.size() == 1);
    CHECK(clipped[0].x == 240);
    // Screenshot blending changes pixels under the box only.
    std::vector<u8> pixels(size_t{1920} * 1080 * 4, 0);
    Overlay::BlendBgra(pixels, 1920, 1080, boxes);
    CHECK(pixels[(size_t{20} * 1920 + 1600) * 4 + 2] != 0);
    CHECK(pixels[(size_t{1000} * 1920 + 100) * 4] == 0);
    // Text is drawn: the toast has bright pixels.
    bool bright = false;
    for (size_t i = 0; i + 3 < toast.rgba.size(); i += 4) bright |= toast.rgba[i] > 200 && toast.rgba[i + 1] > 200;
    CHECK(bright);
    CHECK(Overlay::Layout(frame, {0, 0, 0, 0}, 1920, 1080).empty());
}

TEST_CASE("OotCoopUx: session frame publishes tags from the camera", "[oot_coop]") {
    Ux::Reset();
    const std::string build = "0123456789ABCDEF0123456789ABCDEF01234567";
    Json doc{{"builds", {{build, {{"camera", CameraJson()}}}}}};
    Ux::SessionUx session;
    session.Configure(std::make_shared<const Ux::Config>(Ux::DecodeConfig(doc, build)));
    session.SetName("peer-a", "Anthony");
    const u64 base = 0x1000;
    auto memory = CameraMemory(base, Simple(), false);
    std::vector<std::string> lines;
    const auto log = [&](std::string_view line) { lines.emplace_back(line); };
    session.Frame(memory, base, "SceneA", false, {{"peer-a", "SceneA", false, false, {0, -1.5f, -5}}}, log);
    auto frame = Ux::Current();
    REQUIRE(frame.tags.size() == 1);
    CHECK(frame.tags[0].text == "Anthony");
    CHECK_FALSE(lines.empty()); // the first camera sample is logged
    // The peer leaves our scene: the tags go away at once.
    session.Frame(memory, base, "SceneA", false, {{"peer-a", "SceneB", false, false, {0, 0, -5}}}, log);
    CHECK(Ux::Current().tags.empty());
    // No peers at all: nothing published, nothing read.
    session.Frame(memory, base, "SceneA", false, {}, log);
    CHECK(Ux::Current().tags.empty());
    // Unreadable camera: no tags, a bounded failure line.
    FakeMemory empty;
    const size_t before = lines.size();
    session.Frame(empty, base, "SceneA", false, {{"peer-a", "SceneA", false, false, {0, 0, -5}}}, log);
    CHECK(Ux::Current().tags.empty());
    CHECK(lines.size() == before + 1);
    Ux::Reset();
}
