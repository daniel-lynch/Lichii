// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/nx2_event_trace.h"

namespace {
namespace Trace = Common::Nx2EventTrace;
using Trace::RomfsKind;

std::vector<std::string> Lines(const std::filesystem::path& path) {
    std::vector<std::string> lines;
    std::ifstream in{path};
    for (std::string line; std::getline(in, line);) {
        lines.push_back(line);
    }
    return lines;
}

bool Digits(std::string_view text) {
    return !text.empty() &&
           std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}
} // namespace

TEST_CASE("Event trace switches: off, the main log or a directory", "[common][nx2_event_trace]") {
    REQUIRE(Trace::ParseTarget(nullptr) == Trace::Target{});
    REQUIRE(Trace::ParseTarget("") == Trace::Target{});
    REQUIRE(Trace::ParseTarget("0") == Trace::Target{});
    REQUIRE_FALSE(Trace::ParseTarget("0").On());
    REQUIRE(Trace::ParseTarget("1") == Trace::Target{Trace::Sink::MainLog, {}});
    REQUIRE(Trace::ParseTarget("/tmp/trace") ==
            Trace::Target{Trace::Sink::Directory, "/tmp/trace"});
    // "10" and "01" are directories, not switches.
    REQUIRE(Trace::ParseTarget("10").sink == Trace::Sink::Directory);
    REQUIRE(Trace::ParseTarget("01").sink == Trace::Sink::Directory);
}

TEST_CASE("Event trace matcher: Ocarina's progression files", "[common][nx2_event_trace]") {
    using Trace::RomfsKindOfPath;
    // Spellings from the 1.1.0 RomFS listing.
    REQUIRE(RomfsKindOfPath("/Event/EventFlow/SageOfSpirit.bfevfl.zs") == RomfsKind::EventFlow);
    REQUIRE(RomfsKindOfPath("Event/EventFlow/SageOfSpirit.bfevfl.zs") == RomfsKind::EventFlow);
    REQUIRE(RomfsKindOfPath("/Event/EventFlow/SageOfSpirit.bfevfl") == RomfsKind::EventFlow);
    REQUIRE(RomfsKindOfPath("/Event/EventFlow/Dm_DesertColossus_0030.bfevfl.zs") ==
            RomfsKind::Timeline);
    REQUIRE(RomfsKindOfPath("/Event/EventFlow/Dm_Awake_Fire_Get.110.bfevfl.zs") ==
            RomfsKind::Timeline);
    REQUIRE(RomfsKindOfPath("/Event/Timeline/Anything.bin") == RomfsKind::Timeline);
    REQUIRE(RomfsKindOfPath("/Event/Movie/Dm_FinalBattle_0002.webm") == RomfsKind::Movie);
    REQUIRE(RomfsKindOfPath("/Banc/DesertColossus/DesertColossus_Adult_Static.bcett.byml.zs") ==
            RomfsKind::Banc);
    REQUIRE(RomfsKindOfPath(
                "/Banc/GameBancParam/DesertColossus_Adult.game__banc__GameBancParam.bgyml") ==
            RomfsKind::Banc);
    REQUIRE(RomfsKindOfPath("/Banc/BancBasedResource/StartPos.byml.zs") == RomfsKind::Banc);
    REQUIRE(RomfsKindOfPath("/GameData/GameDataList.Product.110.byml.zs") == RomfsKind::GameData);

    // Not progression files.
    REQUIRE_FALSE(RomfsKindOfPath("/Event/EventFlow/").has_value());
    REQUIRE_FALSE(RomfsKindOfPath("/Event/EventFlow/notes.txt").has_value());
    REQUIRE_FALSE(RomfsKindOfPath("/Event/EventSetting/EventSettingComponentList/"
                                  "TimelineEvent.engine__event__EventSettingComponentList.bgyml")
                      .has_value());
    REQUIRE_FALSE(RomfsKindOfPath("/AS/Dm_DesertColossus_0030.root.asb.zs").has_value());
    REQUIRE_FALSE(RomfsKindOfPath("/Model/Dm_DesertColossus_0030_Player_Child.anim.bfres.zs")
                      .has_value());
    REQUIRE_FALSE(RomfsKindOfPath("/Banc").has_value());
    REQUIRE_FALSE(RomfsKindOfPath("/Banc/").has_value());
    REQUIRE_FALSE(RomfsKindOfPath("/BancX/a.bin").has_value());
    REQUIRE_FALSE(RomfsKindOfPath("/GameDataX/a.bin").has_value());
    REQUIRE_FALSE(RomfsKindOfPath("").has_value());

    REQUIRE(Trace::Name(RomfsKind::EventFlow) == "evfl");
    REQUIRE(Trace::Name(RomfsKind::Timeline) == "timeline");
    REQUIRE(Trace::Name(RomfsKind::Movie) == "movie");
    REQUIRE(Trace::Name(RomfsKind::Banc) == "banc");
    REQUIRE(Trace::Name(RomfsKind::GameData) == "gamedata");
}

TEST_CASE("Event trace hashes are MurmurHash3 x86 32", "[common][nx2_event_trace]") {
    // Reference vectors.
    REQUIRE(Trace::Murmur3("") == 0);
    REQUIRE(Trace::Murmur3("", 1) == 0x514E28B7);
    REQUIRE(Trace::Murmur3("abc") == 0xB3DD93FA);
    REQUIRE(Trace::Murmur3("test") == 0xBA6BD213);
    REQUIRE(Trace::Murmur3("Hello, world!") == 0xC0363E43);
    // The constants the game compares against (FUN_7102cf54a0, vt_7104b21598::vslot3).
    REQUIRE(Trace::Murmur3("Complete") == 0x4C0A63F4);
    REQUIRE(Trace::Murmur3("NotReady") == 0x57B849F0);
    REQUIRE(Trace::Murmur3("HavePlayedEvent") == 0x2B05CBB9);
}

TEST_CASE("Event trace names quest steps by their hash", "[common][nx2_event_trace]") {
    REQUIRE(Trace::NameOfHash(0xEE867C27) == "Step07");
    REQUIRE(Trace::NameOfHash(0xEC4C4A24) == "Step05_01");
    REQUIRE(Trace::NameOfHash(0x3EF71C9D) == "Step07_pre00");
    REQUIRE(Trace::NameOfHash(0x4C0A63F4) == "Complete");
    REQUIRE(Trace::NameOfHash(0x57B849F0) == "NotReady");
    REQUIRE(Trace::NameOfHash(Trace::Murmur3("Ready")) == "Ready");
    REQUIRE(Trace::DescribeHash(0xEE867C27) == "Step07 (0xee867c27)");
    REQUIRE(Trace::DescribeHash(0x00001234) == "0x00001234");
    REQUIRE_FALSE(Trace::NameOfHash(Trace::Murmur3("NotAStep")).has_value());
}

TEST_CASE("Event trace names hashes from a names file", "[common][nx2_event_trace]") {
    const auto path = std::filesystem::temp_directory_path() / "nx2_event_trace_names_test.txt";
    {
        std::ofstream out{path};
        out << "# comment\n\nTraceTest_Alpha\r\nTraceTest_Beta \n";
    }
    REQUIRE(Trace::LoadNamesFile(path.string().c_str()) == 2);
    REQUIRE(Trace::NameOfHash(Trace::Murmur3("TraceTest_Alpha")) == "TraceTest_Alpha");
    REQUIRE(Trace::NameOfHash(Trace::Murmur3("TraceTest_Beta")) == "TraceTest_Beta");
    // Already known: nothing new.
    REQUIRE(Trace::LoadNamesFile(path.string().c_str()) == 0);
    std::filesystem::remove(path);
    REQUIRE(Trace::LoadNamesFile(nullptr) == 0);
    REQUIRE(Trace::LoadNamesFile((path.string() + ".missing").c_str()) == 0);
}

TEST_CASE("Event trace lines carry wall and session time", "[common][nx2_event_trace]") {
    using namespace std::chrono_literals;
    const auto wall = std::chrono::system_clock::now();
    const std::string line = Trace::FormatLine(wall, 612345ms, "romfs evfl /x");
    // "YYYY-MM-DD HH:MM:SS.mmm +612.345 romfs evfl /x"
    REQUIRE(line.size() == 23 + std::string_view{" +612.345 romfs evfl /x"}.size());
    REQUIRE(line.ends_with(" +612.345 romfs evfl /x"));
    REQUIRE(Digits(line.substr(0, 4)));
    REQUIRE(line[4] == '-');
    REQUIRE(line[7] == '-');
    REQUIRE(line[10] == ' ');
    REQUIRE(line[13] == ':');
    REQUIRE(line[16] == ':');
    REQUIRE(line[19] == '.');
    REQUIRE(Digits(line.substr(20, 3)));

    const std::string name = Trace::SessionFileName(wall);
    // "event-trace-YYYYMMDD-HHMMSS-mmm.log"
    REQUIRE(name.starts_with("event-trace-"));
    REQUIRE(name.ends_with(".log"));
    REQUIRE(name.size() == std::string_view{"event-trace-20261106-231004-123.log"}.size());
    REQUIRE(Digits(name.substr(12, 8)));
    REQUIRE(Digits(name.substr(21, 6)));
    REQUIRE(Digits(name.substr(28, 3)));
}

TEST_CASE("Event trace writes one file per session to a directory", "[common][nx2_event_trace]") {
    const auto directory = std::filesystem::temp_directory_path() / "nx2_event_trace_test_dir";
    std::filesystem::remove_all(directory);
    const Trace::Target target = Trace::ParseTarget(directory.string().c_str());

    Trace::BeginSession();
    Trace::Write(target, "first");
    Trace::Write(target, "second");
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator{directory}) {
        files.push_back(entry.path());
    }
    REQUIRE(files.size() == 1);
    // Flushed per line: readable while the session still runs.
    auto lines = Lines(files[0]);
    REQUIRE(lines.size() == 2);
    REQUIRE(lines[0].ends_with(" first"));
    REQUIRE(lines[1].ends_with(" second"));

    std::this_thread::sleep_for(std::chrono::milliseconds{5});
    Trace::BeginSession();
    Trace::Write(target, "third");
    files.clear();
    for (const auto& entry : std::filesystem::directory_iterator{directory}) {
        files.push_back(entry.path());
    }
    REQUIRE(files.size() == 2);
    Trace::BeginSession(); // closes the file
    std::filesystem::remove_all(directory);

    // Off writes nothing and does not throw.
    Trace::Write(Trace::Target{}, "nothing");
}
