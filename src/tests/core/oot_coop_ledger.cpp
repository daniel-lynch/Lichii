// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <catch2/catch_test_macros.hpp>
#include "core/arm/dynarmic/oot_coop_ledger.h"
#include "core/arm/dynarmic/oot_coop_sync.h"

namespace {
using namespace Core::OotCoop;
namespace L = Core::OotCoop::Ledger;
namespace fs = std::filesystem;
using Json = nlohmann::json;
struct Temp {
    fs::path root;
    Temp() {
        static std::atomic<unsigned> serial{};
        root = fs::temp_directory_path() / ("oot-ledger-test-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(++serial));
        fs::create_directories(root);
    }
    ~Temp() { std::error_code ignored; fs::remove_all(root, ignored); }
    void Put(const fs::path& relative, std::string_view text) {
        auto path = root / relative;
        fs::create_directories(path.parent_path());
        std::ofstream out{path, std::ios::binary};
        out << text;
        REQUIRE(out.good());
    }
};
Change Give(u64 seq = 1, std::string sender = "sender") {
    Change change{Kind::Item, std::move(sender), seq, PackItem({1, 1, {}}), 1};
    change.name = "Test_Item_A";
    change.catchup = true;
    change.catchup_transaction = 1;
    return change;
}
Json PageJson() {
    auto first = EncodeChange(Give());
    first["rev"] = 2;
    auto second = EncodeChange(Give(2));
    second["rev"] = 4;
    return {{"type", "catchup"}, {"epoch", "0011223344556677"}, {"floor", 0},
            {"through", 5}, {"after", 0}, {"more", false}, {"grants", Json::array({first, second})}};
}
}

TEST_CASE("Ledger sidecar roundtrip and room replacement", "[oot_coop]") {
    Temp temp;
    const auto path = temp.root / "slot_a/coop-ledger.json";
    L::Entry first{"0011223344556677", 17, 100, {{"sender", 1}, {"other", 4}}};
    L::Write(path, "slot_a", "room/team", first);
    L::Write(path, "slot_a", "other/team", {"8899aabbccddeeff", 3, 90, {{"third", 2}}});
    auto document = L::Read(path, "slot_a");
    REQUIRE_FALSE(document.corrupt);
    REQUIRE(document.rooms.size() == 2);
    auto loaded = L::Find(document, "room/team", first.epoch);
    REQUIRE(loaded);
    CHECK(loaded->through == 17);
    CHECK(loaded->applied == first.applied);
    CHECK_FALSE(L::Find(document, "room/team", "new-epoch"));
    first.applied.push_back({"sender", 5});
    L::Write(path, "slot_a", "room/team", first);
    document = L::Read(path, "slot_a");
    CHECK(document.rooms.at("other/team").applied.size() == 1);
    CHECK(document.rooms.at("room/team").applied.size() == 3);
    CHECK(std::distance(fs::directory_iterator(path.parent_path()), fs::directory_iterator{}) == 1);
}

TEST_CASE("Ledger bounds use room LRU and oldest inserted ID", "[oot_coop]") {
    L::Document document{"slot_a"};
    for (u64 n = 0; n < 10; ++n)
        L::Merge(document, "room" + std::to_string(n) + "/team", {"epoch", n, n, {{"sender", n + 1}}});
    REQUIRE(document.rooms.size() == L::RoomLimit);
    CHECK_FALSE(document.rooms.contains("room0/team"));
    CHECK_FALSE(document.rooms.contains("room1/team"));
    L::Entry entry{"epoch", 9000, 100};
    for (u64 n = 1; n <= L::AppliedLimit + 3; ++n) entry.applied.emplace_back("sender", n);
    L::Merge(document, "room2/team", entry);
    REQUIRE(document.rooms.at("room2/team").applied.size() == L::AppliedLimit);
    CHECK(document.rooms.at("room2/team").applied.front().second == 4);
    // A maximum-size document is deliberately larger than the wire frame bound.
    Temp temp;
    for (u64 n = 2; n < 10; ++n) {
        auto copy = entry;
        for (auto& id : copy.applied) id.first = std::string(64, 's');
        L::Write(temp.root / "large.json", "slot_a", "room" + std::to_string(n) + "/team", std::move(copy));
    }
    CHECK(fs::file_size(temp.root / "large.json") > MaxFrame);
    CHECK_FALSE(L::Read(temp.root / "large.json", "slot_a").corrupt);
}

TEST_CASE("Ledger corrupt sidecar is kept until a save write", "[oot_coop]") {
    Temp temp;
    temp.Put("broken.json", "{not JSON");
    const auto path = temp.root / "broken.json";
    CHECK(L::Read(path, "slot_a").corrupt);
    CHECK_FALSE(fs::exists(temp.root / "broken.json.bad"));
    L::Write(path, "slot_a", "room/team", {"epoch", 2, 10, {{"sender", 1}}});
    CHECK(fs::exists(temp.root / "broken.json.bad"));
    CHECK_FALSE(L::Read(path, "slot_a").corrupt);
    std::ifstream bad{temp.root / "broken.json.bad"};
    std::string contents{std::istreambuf_iterator<char>{bad}, {}};
    CHECK(contents == "{not JSON");
    CHECK(L::Read(path, "different_slot").corrupt);
    temp.Put("duplicate.json", "{\"format\":1,\"format\":1,\"slot\":\"slot_a\",\"rooms\":{}}");
    CHECK(L::Read(temp.root / "duplicate.json", "slot_a").corrupt);
}

TEST_CASE("Ledger admission prioritizes receipts and live queue before covered flags", "[oot_coop]") {
    L::Book book;
    book.Epoch("epoch");
    book.Begin(1, "epoch", 0, 9);
    book.Bind(L::Entry{"epoch", 3, 0, {{"sender", 1}}});
    auto change = Give();
    CHECK(book.Admit(change) == L::Decision::Applied);
    change = Give(2);
    change.catchup = false;
    CHECK(book.Admit(change) == L::Decision::Admit);
    change.catchup = true;
    CHECK(book.Admit(change) == L::Decision::Queued);
    book.Applied(change);
    CHECK(book.Admit(change) == L::Decision::Applied);
    auto chest = Give(3);
    chest.source = World::Origin::Chest(7);
    book.LocalFlag({Kind::Bool, {}, 0, 7, 1});
    CHECK(book.Admit(chest) == L::Decision::Covered);
    auto bit = Give(4);
    bit.source = World::Origin{World::OriginKind::Bit, 8, 0};
    book.LocalFlag({Kind::Bit, {}, 0, 8, 1});
    CHECK(book.Admit(bit) == L::Decision::Covered);
    auto token = Give(5);
    token.source = World::Origin{World::OriginKind::Token, 9, 10};
    book.LocalFlag({Kind::Bit, {}, 0, 9, 1});
    CHECK(book.Admit(token) == L::Decision::Admit); // Both token flags are required.
    book.Dropped(token);
    book.LocalFlag({Kind::Bit, {}, 0, 10, 1});
    CHECK(book.Admit(token) == L::Decision::Covered);
    auto unknown = Give(6);
    CHECK(book.Admit(unknown) == L::Decision::Admit);
    book.Applied(unknown);
    book.Through(9);
    CHECK(book.Snapshot().through == 9);
    book.End(1);
    auto live = Give(7);
    live.catchup = false;
    CHECK(book.Admit(live, false) == L::Decision::Policy);
}

TEST_CASE("Ledger own IDs survive bind; an epoch reset forgets old campaign receipts", "[oot_coop]") {
    L::Book book;
    auto own = Give(2, "me");
    book.Applied(own);
    book.Epoch("epoch");
    book.Bind(L::Entry{"epoch", 1, 0, {{"other", 1}}});
    CHECK(book.Admit(own) == L::Decision::Applied);
    CHECK(book.Snapshot().applied.size() == 2);
    book.Epoch("changed");
    CHECK(book.Snapshot().applied.empty());
    CHECK(book.Snapshot().through == 0);
    CHECK(book.Admit(own) == L::Decision::Admit);
    for (u64 n = 1; n <= L::AppliedLimit + 1; ++n) book.Applied(Give(n, "bound"));
    CHECK(book.Snapshot().applied.size() == L::AppliedLimit);
    CHECK(book.Snapshot().applied.front().second == 2);
}

TEST_CASE("Ledger regex matches the whole virtual path and one safe slot", "[oot_coop]") {
    Temp temp;
    L::Pattern pattern{R"(/(slot_[a-z]+)/data\.bin)"};
    REQUIRE(pattern.Match("/slot_a/data.bin"));
    CHECK(*pattern.Match("/slot_a/data.bin") == "slot_a");
    CHECK_FALSE(pattern.Match("prefix/slot_a/data.bin"));
    CHECK_FALSE(pattern.Match("/slot_a/data.bin/extra"));
    CHECK_THROWS(L::Pattern{"/slot_a/data.bin"});
    CHECK_THROWS(L::Pattern{"/(slot_(a))/data.bin"});
    CHECK_FALSE(L::Pattern{"/(.*)/data.bin"}.Match("/../data.bin"));
    fs::create_directories(temp.root / "slot_a");
    CHECK(L::SidecarPath((temp.root / "slot_a/data.bin").string(), "slot_a", temp.root) == temp.root / "slot_a/coop-ledger.json");
    CHECK(L::SidecarPath("slot_a/data.bin", "slot_a", temp.root) == temp.root / "oot-coop-ledger/slot_a.json");
    CHECK(L::SidecarPath("", "slot_a", temp.root) == temp.root / "oot-coop-ledger/slot_a.json");
    CHECK(L::SidecarPath((temp.root / "missing/data.bin").string(), "slot_a", temp.root) == temp.root / "oot-coop-ledger/slot_a.json");
    temp.Put("keys.json", "{}");
    temp.Put("coop-ledger.json", R"({"version":1,"save_file":"/(slot_[a-z]+)/data\\.bin"})");
    std::string why;
    CHECK(L::LoadConfig(temp.root / "keys.json", why));
    temp.Put("coop-ledger.json", R"json({"version":1,"save_file":"/(.*)","unknown":true})json");
    CHECK_FALSE(L::LoadConfig(temp.root / "keys.json", why));
}

TEST_CASE("Ledger binds last read after 60 consecutive frames and posts writes", "[oot_coop]") {
    Temp temp;
    fs::create_directories(temp.root / "slot_a");
    const auto host = (temp.root / "slot_a/data.bin").string();
    L::Write(temp.root / "slot_a/coop-ledger.json", "slot_a", "room/team", {"epoch", 8, 1, {{"sender", 1}}});
    L::Book book;
    std::vector<std::function<void()>> posted;
    {
        L::Save save{book, L::Pattern{R"(/(slot_[a-z]+)/data\.bin)"}, temp.root, "room/team",
                     [&](std::function<void()> work) { posted.push_back(std::move(work)); }};
        save.Epoch("epoch");
        L::NoteSaveFile("/slot_a/data.bin", host, true); // read-write load is not a save receipt
        CHECK(posted.empty());
        L::NoteSaveFile("/slot_a/data.bin", host, false);
        for (int i = 0; i < 59; ++i) save.Frame(true);
        CHECK_FALSE(save.Bound());
        save.Frame(false);
        for (int i = 0; i < 60; ++i) save.Frame(true);
        REQUIRE(save.Bound());
        CHECK(book.Admit(Give()) == L::Decision::Applied);
        book.Applied(Give(2));
        L::NoteSaveFile("/slot_a/data.bin", host, true);
        REQUIRE(posted.size() == 1);
        CHECK(L::Read(temp.root / "slot_a/coop-ledger.json", "slot_a").rooms.at("room/team").applied.size() == 1);
        posted[0]();
        CHECK(L::Read(temp.root / "slot_a/coop-ledger.json", "slot_a").rooms.at("room/team").applied.size() == 2);
        L::NoteSaveFile("/slot_b/data.bin", {}, false);
        CHECK_FALSE(save.Enabled());
    }
    L::NoteSaveFile("/slot_a/data.bin", host, true); // destructor removed its callback
    CHECK(posted.size() == 1);
}

TEST_CASE("Ledger new save binds empty and first rename uses fallback", "[oot_coop]") {
    Temp temp;
    L::Book book;
    std::vector<std::function<void()>> posted;
    L::Save save{book, L::Pattern{R"(/(slot_[a-z]+)/data\.bin)"}, temp.root, "room/team",
                 [&](std::function<void()> work) { posted.push_back(std::move(work)); }};
    save.Epoch("epoch");
    for (int i = 0; i < 60; ++i) save.Frame(true);
    REQUIRE(save.Bound());
    book.Applied(Give());
    save.Note("/slot_a/data.bin", {}, true);
    REQUIRE(posted.size() == 1);
    posted[0]();
    CHECK(fs::exists(temp.root / "oot-coop-ledger/slot_a.json"));
}

TEST_CASE("Ledger canonical mod hash matches Python synthetic fixture", "[oot_coop]") {
    Temp temp;
    temp.Put("keys.json", "{}\n");
    temp.Put("coop-ux.json", "{\"version\":1}\n");
    temp.Put("0000000000000001/Alpha/romfs/a.bin", "abc");
    temp.Put("0000000000000001/Beta/exefs/b.bin", "xyz\n");
    temp.Put("0000000000000001/Alpha/cheats/local.txt", "ignored");
    const auto title = temp.root / "0000000000000001";
    auto hash = L::ModHash(temp.root / "keys.json", title);
    CHECK(hash.files == 2);
    CHECK(hash.bytes == 24);
    CHECK(hash.digest == "2d664dfd842f28a77e40468dbc1b732493b854d9f52fb91945fc4f09e240867e");
    CHECK(hash.text.starts_with("oot-coop-mod-hash 1\nkeys 3 "));
    CHECK(hash.text.find("sidecar coop-ledger.json absent\nmod exefs/b.bin 4 ") != std::string::npos);
    CHECK(hash.text.back() == '\n');
    fs::rename(title / "Alpha", title / "Renamed");
    CHECK(L::ModHash(temp.root / "keys.json", title).text == hash.text);
    CHECK_THROWS(L::ModHash(temp.root / "missing.json", title));
}

TEST_CASE("Ledger catchup pages reject bad epochs revisions classes and envelopes", "[oot_coop]") {
    const auto good = PageJson();
    auto parsed = L::DecodePage(good.dump(), "0011223344556677", 0);
    REQUIRE(parsed);
    REQUIRE(parsed->grants.size() == 2);
    CHECK(parsed->grants[0].catchup);
    CHECK_FALSE(EncodeChange(parsed->grants[0]).contains("catchup"));
    CHECK_FALSE(L::DecodePage(good.dump(), "wrong", 0));
    CHECK_FALSE(L::DecodePage(good.dump(), "0011223344556677", 1));
    auto check_bad = [&](Json bad) { CHECK_FALSE(L::DecodePage(bad.dump(), "0011223344556677", 0)); };
    auto bad = good; bad["grants"][1]["rev"] = 2; check_bad(bad);
    bad = good; bad["grants"][0]["rev"] = 0; check_bad(bad);
    bad = good; bad["through"] = 3; check_bad(bad);
    bad = good; bad["floor"] = 2; check_bad(bad);
    bad = good; bad["after"] = true; check_bad(bad);
    bad = good; bad["extra"] = 1; check_bad(bad);
    bad = good; bad["grants"] = Json::array(); bad["more"] = true; check_bad(bad);
    bad = good; bad["grants"][0] = {{"type", "bool_set"}, {"origin", "sender"}, {"seq", 1}, {"hash", 7}, {"value", true}, {"rev", 2}}; check_bad(bad);
    CHECK(L::DecodeCapability(R"({"v":1,"epoch":"epoch_1","floor":0})"));
    CHECK_FALSE(L::DecodeCapability(R"({"v":2,"epoch":"epoch_1","floor":0})"));
    CHECK_FALSE(L::DecodeCapability(R"({"v":1,"epoch":"bad/epoch","floor":0})"));
    CHECK_FALSE(L::DecodeCapability(R"({"v":1,"epoch":"epoch","floor":true})"));
}
