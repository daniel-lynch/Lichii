// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <map>
#include <catch2/catch_test_macros.hpp>
#include "core/arm/dynarmic/oot_coop_checks.h"
#include "core/arm/dynarmic/oot_coop_ux.h"
#include "video_core/coop_checks_overlay.h"
#include "video_core/coop_overlay.h"

namespace {
namespace Checks = Core::OotCoop::Checks;
namespace Ux = Core::OotCoop::Ux;
namespace Overlay = VideoCore::CoopOverlay;
using Json = nlohmann::json;
using Checks::FlagKind;
using Checks::Style;

// Synthetic values only: scene "Alpha" (two age variants), "Beta"; made-up keys.
Json Check(std::string id, std::string name, std::vector<std::string> bancs, Json flag, bool shuffled = true) {
    Json c{{"id", id}, {"name", name}, {"area", "Alpha Woods"}, {"bancs", bancs}, {"shuffled", shuffled},
           {"kind", "chest"}};
    if (!flag.is_null()) c["flag"] = flag;
    else c["why"] = "no flag";
    return c;
}
Json Document() {
    return {{"schema", 1}, {"seed", "unit-seed"}, {"file_hash", "00112233aabbccdd"},
            {"areas", {{"Alpha_Child", "Alpha Woods"}, {"Alpha_Adult", "Alpha Woods"}, {"Beta", "Beta Cave"}}},
            {"checks", {
                Check("c:1", "Chest one", {"Alpha_Child", "Alpha_Adult"}, {{"bool", "0x00000011"}}),
                Check("c:2", "Chest two", {"Alpha_Child"}, {{"bool", "0x00000012"}}),
                Check("c:3", "Npc gift", {"Alpha_Child"}, nullptr),
                Check("c:4", "Token spot", {"Alpha_Child"}, {{"bit", "0x0000000000000014"}}, false),
                Check("c:5", "Adult chest", {"Alpha_Adult"}, {{"bool", "0x00000015"}}),
                Check("c:6", "Cave chest", {"Beta"}, {{"bit", "0xfedcba9876543210"}}),
            }}};
}
Checks::Sidecar Sidecar() {
    auto s = Checks::Decode(Document());
    REQUIRE(s);
    return *s;
}
std::vector<std::string> Texts(const std::vector<Checks::Row>& rows) {
    std::vector<std::string> out;
    for (const auto& r : rows) out.push_back(r.text);
    return out;
}
} // namespace

TEST_CASE("OotCoopChecks: sidecar decode", "[oot_coop]") {
    std::string why;
    const auto s = Checks::Decode(Document(), &why);
    REQUIRE(s);
    CHECK(why.empty());
    REQUIRE(s->checks.size() == 6);
    CHECK(s->seed == "unit-seed");
    CHECK(s->checks[0].flag == Checks::Flag{FlagKind::Bool, 0x11});
    CHECK(s->checks[5].flag == Checks::Flag{FlagKind::Bit, 0xfedcba9876543210ULL});
    CHECK_FALSE(s->checks[2].Tracked());
    CHECK_FALSE(s->checks[3].shuffled);
    CHECK(s->checks[0].In("Alpha_Adult"));
    CHECK_FALSE(s->checks[1].In("Alpha_Adult"));
    CHECK(s->areas.at("Beta") == "Beta Cave");

    SECTION("bad checks are skipped, the rest load") {
        auto d = Document();
        d["checks"].push_back(Check("c:7", "Too wide bool", {"Beta"}, {{"bool", "0x100000000"}}));
        d["checks"].push_back(Check("c:8", "Zero key", {"Beta"}, {{"bit", "0x0"}}));
        d["checks"].push_back(Check("c:9", "Two flags", {"Beta"}, {{"bool", "0x1"}, {"bit", "0x2"}}));
        d["checks"].push_back(Check("c:10", "Not hex", {"Beta"}, {{"bool", "17"}}));
        d["checks"].push_back(Check("c:11", "No banc", {}, {{"bool", "0x1"}}));
        d["checks"].push_back(Check("c:1", "Duplicate id", {"Beta"}, {{"bool", "0x1"}}));
        auto bad = Check("c:12", "Shuffled not bool", {"Beta"}, {{"bool", "0x1"}});
        bad["shuffled"] = "yes";
        d["checks"].push_back(bad);
        d["extra"] = "ignored";
        const auto t = Checks::Decode(d, &why);
        REQUIRE(t);
        CHECK(t->checks.size() == 6);
        CHECK(why == "7 bad checks skipped");
    }
    SECTION("bad documents") {
        CHECK_FALSE(Checks::Decode(Json::array()));
        auto d = Document();
        d["schema"] = 2;
        CHECK_FALSE(Checks::Decode(d, &why));
        CHECK(why == "schema is not 1");
        d = Document();
        d["checks"] = Json::array();
        CHECK_FALSE(Checks::Decode(d, &why));
        d.erase("checks");
        CHECK_FALSE(Checks::Decode(d));
    }
    SECTION("names are printable ASCII, bounded") {
        auto d = Document();
        d["checks"][0]["name"] = std::string(80, 'x') + "\x01";
        d["checks"][1]["name"] = "Caf\xc3\xa9";
        const auto t = Checks::Decode(d);
        REQUIRE(t);
        CHECK(t->checks[0].name.size() == Checks::NameLimit);
        CHECK(t->checks[1].name == "Caf??");
    }
}

TEST_CASE("OotCoopChecks: spoiler file must match the checks file", "[oot_coop]") {
    auto s = Sidecar();
    std::string why;
    Json spoiler{{"schema", 1}, {"file_hash", "ffff"}, {"items", {{"c:1", "Sword"}}}};
    CHECK_FALSE(Checks::DecodeSpoiler(spoiler, s, &why));
    CHECK(why == "file_hash does not match the checks file");
    CHECK(s.items.empty());
    spoiler["file_hash"] = "00112233aabbccdd";
    spoiler["items"]["unknown"] = "Ignored";
    spoiler["items"]["c:2"] = 7;
    CHECK(Checks::DecodeSpoiler(spoiler, s, &why));
    CHECK(s.items.size() == 1);
    CHECK(s.items.at("c:1") == "Sword");
}

TEST_CASE("OotCoopChecks: trigger options", "[oot_coop]") {
    CHECK(Checks::ParseOptions("") == Checks::Options{});
    CHECK(Checks::ParseOptions("all") == Checks::Options{true, false, false, false});
    CHECK(Checks::ParseOptions(" spoiler,vanilla\n") == Checks::Options{false, true, true, false});
    CHECK(Checks::ParseOptions("dense bogus") == Checks::Options{true, false, false, true});
    CHECK(Checks::ParseOptions(std::string(300, ' ') + "all") == Checks::Options{});
}

TEST_CASE("OotCoopChecks: state evaluation and counts", "[oot_coop]") {
    Checks::Model m{Sidecar()};
    std::map<u64, std::optional<bool>> flags{{0x11, true}, {0x12, false}, {0x14, true}, {0x15, std::nullopt},
                                             {0xfedcba9876543210ULL, true}};
    std::vector<Checks::Flag> asked;
    m.Evaluate([&](const Checks::Flag& f) {
        asked.push_back(f);
        return flags.at(f.key);
    });
    CHECK(asked.size() == 5); // the untracked check is never read
    CHECK(m.StateOf(0) == Checks::State::Done);
    CHECK(m.StateOf(1) == Checks::State::Open);
    CHECK(m.StateOf(2) == Checks::State::Untracked);
    CHECK(m.StateOf(4) == Checks::State::Open); // unreadable: initial state kept
    CHECK(m.StateOf(99) == Checks::State::Untracked);

    CHECK(m.Here("Alpha_Child", false) == Checks::Counts{1, 2, 1});
    CHECK(m.Here("Alpha_Child", true) == Checks::Counts{2, 3, 1});   // + the vanilla token spot
    CHECK(m.Here("Alpha_Adult", false) == Checks::Counts{1, 2, 0});
    CHECK(m.Here("Nowhere", false) == Checks::Counts{});
    CHECK(m.Total(false) == Checks::Counts{2, 4, 1});
    CHECK(m.Total(true) == Checks::Counts{3, 5, 1});

    // An unreadable flag keeps the last known value, a readable one replaces it.
    flags[0x11] = std::nullopt;
    flags[0x15] = true;
    m.Evaluate([&](const Checks::Flag& f) { return flags.at(f.key); });
    CHECK(m.StateOf(0) == Checks::State::Done);
    CHECK(m.StateOf(4) == Checks::State::Done);
    flags[0x11] = false; // a reloaded save can clear a flag
    m.Evaluate([&](const Checks::Flag& f) { return flags.at(f.key); });
    CHECK(m.StateOf(0) == Checks::State::Open);
    m.Set(2, true); // untracked stays untracked
    CHECK(m.StateOf(2) == Checks::State::Untracked);
}

TEST_CASE("OotCoopChecks: rows and filters", "[oot_coop]") {
    Checks::Model m{Sidecar()};
    m.Set(0, true);
    const Checks::Options plain{};
    auto rows = m.Rows("Alpha_Child", false, plain);
    CHECK(Texts(rows) == std::vector<std::string>{"Alpha Woods  1/2  total 1/4", "[ ] Chest two", "[?] Npc gift"});
    CHECK(rows[0].style == Style::Title);
    CHECK(rows[1].style == Style::Open);
    CHECK(rows[2].style == Style::Untracked);

    rows = m.Rows("Alpha_Child", false, {true, false, false, false});
    CHECK(Texts(rows).back() == "[x] Chest one");
    CHECK(rows.back().style == Style::Done);
    rows = m.Rows("Alpha_Child", false, {true, false, true, false});
    CHECK(Texts(rows) == std::vector<std::string>{"Alpha Woods  1/3  total 1/5", "[ ] Chest two", "[ ] Token spot",
                                                  "[?] Npc gift", "[x] Chest one"});

    m.Set(4, true);
    rows = m.Rows("Alpha_Adult", false, plain);
    CHECK(Texts(rows) == std::vector<std::string>{"Alpha Woods  2/2  total 2/4", "All 2 done here"});
    CHECK(rows.back().style == Style::Note);

    CHECK(Texts(m.Rows("", false, plain)) == std::vector<std::string>{"Checks  total 2/4"});
    CHECK(Texts(m.Rows("Alpha_Child", true, plain)) == std::vector<std::string>{"Loading  total 2/4"});
    CHECK(Texts(m.Rows("Gamma_Child", false, plain)) ==
          std::vector<std::string>{"Gamma_Child  0/0  total 2/4", "No checks in this area"});
}

TEST_CASE("OotCoopChecks: spoiler text and row budget", "[oot_coop]") {
    auto d = Document();
    for (int i = 0; i < 14; ++i)
        d["checks"].push_back(Check(fmt::format("x:{:02}", i), fmt::format("Extra chest {:02}", i), {"Beta"},
                                    {{"bool", fmt::format("0x{:x}", 0x100 + i)}}));
    auto s = Checks::Decode(d);
    REQUIRE(s);
    REQUIRE(Checks::DecodeSpoiler({{"schema", 1}, {"file_hash", "00112233aabbccdd"},
                                   {"items", {{"c:6", "Very Long Item Name Here"}}}}, *s));
    Checks::Model m{*s};
    auto rows = m.Rows("Beta", false, {});
    REQUIRE(rows.size() == 11);                       // title + 9 checks + "+N more"
    CHECK(rows.back().text == "  +6 more");
    CHECK(rows[1].text == "[ ] Cave chest");          // no spoiler unless asked
    for (const auto& r : rows) CHECK(r.text.size() <= 36);
    rows = m.Rows("Beta", false, {false, true, false, false});
    CHECK(rows[1].text == "[ ] Cave chest - Very Long Item Name"); // cut to the narrow width
    rows = m.Rows("Beta", false, {true, true, false, true}); // dense: 52 wide, 22 rows
    CHECK(rows.size() == 16);
    CHECK(rows[1].text == "[ ] Cave chest - Very Long Item Name Here");
    for (const auto& r : rows) CHECK(r.text.size() <= 52);
}

TEST_CASE("OotCoopChecks: narrow spoiler rows are cut to width", "[oot_coop]") {
    auto s = Sidecar();
    REQUIRE(Checks::DecodeSpoiler({{"schema", 1}, {"file_hash", "00112233aabbccdd"},
                                   {"items", {{"c:2", "Item With A Rather Long Name"}}}}, s));
    Checks::Model m{s};
    const auto rows = m.Rows("Alpha_Child", false, {false, true, false, false});
    REQUIRE(rows.size() == 4); // title, Chest one, Chest two, Npc gift
    CHECK(rows[1].text == "[ ] Chest one");
    CHECK(rows[2].text.size() == 36);
    CHECK(rows[2].text.starts_with("[ ] Chest two - Item"));
}

TEST_CASE("OotCoopChecks: published panel reaches the overlay frame", "[oot_coop]") {
    Ux::Reset();
    CHECK(Ux::Current().Empty());
    Checks::Publish({{"Alpha  0/1", Style::Title}, {"[ ] One", Style::Open}}, false);
    const auto first = Checks::Current();
    REQUIRE(first);
    Checks::Publish({{"Alpha  0/1", Style::Title}, {"[ ] One", Style::Open}}, false);
    CHECK(Checks::Current() == first); // unchanged rows: same immutable panel
    auto frame = Ux::Current();
    CHECK_FALSE(frame.Empty());
    REQUIRE(frame.checks == first);
    Checks::Publish({{"Alpha  1/1", Style::Title}}, false);
    CHECK(Checks::Current()->generation > first->generation);
    Checks::Publish({}, false);
    CHECK_FALSE(Checks::Current());
    Checks::Publish({{"x", Style::Note}}, true);
    Ux::Reset(); // a session reset clears the checks panel too
    CHECK_FALSE(Checks::Current());
    CHECK(Ux::Current().Empty());
}

TEST_CASE("OotCoopChecks: raster box and layout", "[oot_coop]") {
    Checks::Panel panel{{{"Alpha Woods  1/2  total 1/4", Style::Title}, {"[ ] Chest two", Style::Open},
                         {"[x] Chest one", Style::Done}}, false, 7};
    const Overlay::Rect screen{0, 0, 1920, 1080};
    const auto box = Overlay::ChecksBox(panel, 1, screen);
    REQUIRE(box);
    CHECK(box->width == 27 * 14 + 16);
    CHECK(box->height == 3 * (31 + 3) + 8);
    CHECK(box->x + static_cast<s32>(box->width) == 1920 - 16); // right edge
    CHECK(box->y == 1080 * 24 / 100);
    CHECK(box->rgba.size() == size_t{box->width} * box->height * 4);
    CHECK(box->rgba[3] == 255); // opaque border
    CHECK_FALSE(Overlay::ChecksBox(Checks::Panel{}, 1, screen));
    const auto big = Overlay::ChecksBox(panel, 2, {0, 0, 3840, 2160});
    REQUIRE(big);
    CHECK(big->width == 2 * box->width);

    Overlay::ChecksCache cache;
    const auto a = cache.Get(panel, 1, screen);
    CHECK(cache.Get(panel, 1, screen) == a);
    panel.generation = 8;
    CHECK(cache.Get(panel, 1, screen) != a);

    Ux::OverlayFrame frame;
    frame.checks = std::make_shared<const Checks::Panel>(panel);
    const auto boxes = Overlay::Layout(frame, screen, 1920, 1080);
    REQUIRE(boxes.size() == 1);
    CHECK(boxes[0].x == box->x);
    CHECK(boxes[0].width == box->width);
    // Offscreen rectangle: clipped away by Layout.
    CHECK(Overlay::Layout(frame, {0, 0, 1920, 1080}, 100, 100).empty());

    // Toasts start a quarter down at the right edge too: with the panel shown they sit beside it.
    frame.toasts = {Ux::PlainToast("Anthony found a rather long item name here"), Ux::PlainToast("second")};
    frame.more = 2;
    const auto with = Overlay::Layout(frame, screen, 1920, 1080);
    REQUIRE(with.size() == 4); // panel, two toasts, "+2 more"
    for (size_t i = 1; i < with.size(); ++i) {
        CHECK(with[i].x + static_cast<s32>(with[i].width) == box->x - 8);
        CHECK(with[i].x >= 0);
    }
    // 720p, dense-width panel: the column still fits left of it.
    Checks::Panel wide{{{std::string(52, 'w'), Style::Title}}, true, 9};
    frame.checks = std::make_shared<const Checks::Panel>(wide);
    const auto small = Overlay::Layout(frame, {0, 0, 1280, 720}, 1280, 720);
    REQUIRE(small.size() == 4);
    for (size_t i = 1; i < small.size(); ++i) {
        CHECK(small[i].x + static_cast<s32>(small[i].width) == small[0].x - 8);
        CHECK(small[i].x >= 0);
    }
}

TEST_CASE("OotCoopChecks: trigger path", "[oot_coop]") {
    std::map<std::string, std::string, std::less<>> env;
    const auto get = [&](std::string_view name) {
        const auto it = env.find(name);
        return it == env.end() ? std::string{} : it->second;
    };
    CHECK(Checks::TriggerPath(get).empty());
    env["EDEN_NX2_OOT_COOP_KEYS"] = "/kit/keys.json";
    CHECK(Checks::TriggerPath(get) == "/kit/coop-checks.show");
    env["EDEN_NX2_OOT_COOP_CHECKS_TRIGGER"] = "/elsewhere/t";
    CHECK(Checks::TriggerPath(get) == "/elsewhere/t");
}
