// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <fstream>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/nx2_switch.h"
#include "video_core/pipeline_diagnostics.h"

using namespace std::chrono_literals;
using VideoCommon::PipelineDiagnostics::SceneMarker;
using VideoCommon::PipelineDiagnostics::WaitCounter;

TEST_CASE("Compute wait samples include outstanding time without counting it twice",
          "[pipeline_diagnostics]") {
    WaitCounter counter;
    const auto start = WaitCounter::Clock::time_point{};
    REQUIRE(counter.Read(start).total == 0ns);
    const u64 first = counter.Begin(start);
    const auto window1 = counter.Read(start + 10s);
    REQUIRE(window1.total == 10s);
    REQUIRE(window1.active == 1);
    const auto window2 = counter.Read(start + 20s);
    REQUIRE(window2.total - window1.total == 10s);
    counter.End(first, start + 23s);
    const auto shutdown = counter.Read(start + 30s);
    REQUIRE(shutdown.total - window2.total == 3s);
    REQUIRE(shutdown.total == 23s);
    REQUIRE(shutdown.active == 0);
    REQUIRE(counter.Read(start + 40s).total == shutdown.total);
}

TEST_CASE("Compute waits count short and overlapping waits independently", "[pipeline_diagnostics]") {
    WaitCounter counter;
    const auto start = WaitCounter::Clock::time_point{};
    const u64 first = counter.Begin(start);
    const u64 second = counter.Begin(start + 2ms);
    REQUIRE(counter.Read(start + 5ms).total == 8ms);
    REQUIRE(counter.Read(start + 5ms).active == 2);
    counter.End(first, start + 6ms);
    counter.End(second, start + 7ms);
    REQUIRE(counter.Read(start + 10ms).total == 11ms);
    REQUIRE(counter.Read(start + 10ms).active == 0);
    counter.End(second, start + 20ms);
    REQUIRE(counter.Read(start + 20ms).total == 11ms);
}

TEST_CASE("Diagnostic boolean switches reject unknown words and honor off values",
          "[pipeline_diagnostics]") {
    for (const auto* name : {"EDEN_NX2_COMPUTE_REBUILD_LOG", "EDEN_NX2_TRANSLATION_MISS_LOG",
                             "EDEN_NX2_COMPUTE_NOINLINE", "EDEN_NX2_COMPUTE_NULL_SLOT_REBUILD",
                             "EDEN_NX2_PIPELINE_DIFF"}) {
        CAPTURE(name);
        REQUIRE_FALSE(Shader::ParseNx2Switch(name, nullptr).has_value());
        for (const auto* value : {"0", "off", "false", "none", " OFF\n", "False", "bogus", "2"}) {
            CAPTURE(value);
            REQUIRE(Shader::ParseNx2Switch(name, value) == false);
        }
        for (const auto* value : {"", "1", "on", "true", " ON\n", "TRUE"}) {
            CAPTURE(value);
            REQUIRE(Shader::ParseNx2Switch(name, value) == true);
        }
    }
}

TEST_CASE("Scene markers detect content changes and keep each marker on one log line",
          "[pipeline_diagnostics]") {
    const auto path = std::filesystem::temp_directory_path() / "lichii-diag-mark-test.txt";
    std::filesystem::remove(path);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::filesystem::remove(path); }
    } cleanup{path};
    SceneMarker marker{path};
    REQUIRE_FALSE(marker.Poll());
    const auto write = [&](const std::string& value) {
        std::ofstream out{path, std::ios::binary | std::ios::trunc};
        out << value;
        REQUIRE(out.good());
    };
    write("boss cutscene\n");
    REQUIRE(marker.Poll() == "boss cutscene\\n");
    REQUIRE_FALSE(marker.Poll());
    write("field scenery\n"); // Same length; no reliance on mtime granularity.
    REQUIRE(marker.Poll() == "field scenery\\n");
    write("line1\r\nline2\\x");
    REQUIRE(marker.Poll() == "line1\\r\\nline2\\\\x");
    write("");
    REQUIRE(marker.Poll() == "");
    REQUIRE_FALSE(marker.Poll());
    std::filesystem::remove(path);
    REQUIRE_FALSE(marker.Poll());
    write("back");
    REQUIRE(marker.Poll() == "back");
    REQUIRE_FALSE(SceneMarker{std::filesystem::temp_directory_path()}.Poll());
    REQUIRE_FALSE(SceneMarker{{}}.Poll());
}

TEST_CASE("Scene marker log size is bounded", "[pipeline_diagnostics]") {
    const auto path = std::filesystem::temp_directory_path() / "lichii-diag-long-mark-test.txt";
    {
        std::ofstream out{path};
        out << std::string(5000, 'a');
    }
    SceneMarker marker{path};
    REQUIRE(marker.Poll() == std::string(4096, 'a') + " [truncated at 4096 bytes]");
    REQUIRE_FALSE(marker.Poll());
    std::filesystem::remove(path);
}

TEST_CASE("Scene marker paths support explicit off words", "[pipeline_diagnostics]") {
    using VideoCommon::PipelineDiagnostics::MarkFilePath;
    REQUIRE(MarkFilePath(nullptr).empty());
    for (const auto* word : {"", " ", "0", "off", "FALSE", " none\n"}) {
        REQUIRE(MarkFilePath(word).empty());
    }
    REQUIRE(MarkFilePath("C:/scene marker.txt") == "C:/scene marker.txt");
    REQUIRE(MarkFilePath("./off") == "./off");
}
