// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <chrono>
#include <string>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "video_core/nx2_profile.h"

namespace {

using namespace VideoCommon;
using Nx2Profile::Zone;

constexpr size_t Index(Zone zone) {
    return static_cast<size_t>(zone);
}

/// Sets the profile's flags for one test and puts them back after.
class Flags {
public:
    Flags(bool profile, bool waits)
        : enabled{Nx2Profile::g_enabled}, waits_{Nx2Profile::g_waits},
          zones{Nx2Profile::g_zones_enabled}, wait_zones{Nx2Profile::g_wait_zones_enabled},
          report{Nx2Profile::g_report_enabled} {
        Nx2Profile::g_enabled = profile;
        Nx2Profile::g_waits = waits;
        Nx2Profile::g_zones_enabled = profile;
        Nx2Profile::g_wait_zones_enabled = profile || waits;
        Nx2Profile::g_report_enabled = profile || waits;
    }
    ~Flags() {
        Nx2Profile::g_enabled = enabled;
        Nx2Profile::g_waits = waits_;
        Nx2Profile::g_zones_enabled = zones;
        Nx2Profile::g_wait_zones_enabled = wait_zones;
        Nx2Profile::g_report_enabled = report;
    }

private:
    bool enabled;
    bool waits_;
    bool zones;
    bool wait_zones;
    bool report;
};

/// Runs `body` on a new thread (its own Nx2Profile::t_data) and returns that thread's totals.
template <typename Body>
Nx2Profile::ThreadData OnThread(Body body) {
    Nx2Profile::ThreadData data;
    std::thread{[&data, &body] {
        body();
        data = Nx2Profile::t_data;
    }}.join();
    return data;
}

void Sleep(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds{ms});
}

} // Anonymous namespace

static_assert(Nx2Profile::IsWaitZone(Zone::Idle));
static_assert(Nx2Profile::IsWaitZone(Zone::SplitDrain));
static_assert(Nx2Profile::IsWaitZone(Zone::SplitWait));
static_assert(Nx2Profile::IsWaitZone(Zone::IdleHook));
static_assert(!Nx2Profile::IsWaitZone(Zone::Push));
static_assert(!Nx2Profile::IsWaitZone(Zone::Method));
static_assert(!Nx2Profile::IsWaitZone(Zone::Draw));

TEST_CASE("Nx2Profile waits: only wait zones record, nested in untimed work", "[video_core]") {
    const Flags flags{false, true};
    REQUIRE_FALSE(Nx2Profile::Enabled());
    const auto data = OnThread([] {
        const Nx2Profile::Scope push{Zone::Push};
        Sleep(1);
        {
            const Nx2Profile::Scope drain{Zone::SplitDrain};
            Sleep(2);
        }
        const Nx2Profile::Scope macro{Zone::Macro};
        const Nx2Profile::Scope writer{Zone::WriterWait};
        Sleep(1);
    });
    REQUIRE(data.count[Index(Zone::Push)] == 0);
    REQUIRE(data.exclusive_ns[Index(Zone::Push)] == 0);
    REQUIRE(data.count[Index(Zone::Macro)] == 0);
    REQUIRE(data.count[Index(Zone::SplitDrain)] == 1);
    REQUIRE(data.exclusive_ns[Index(Zone::SplitDrain)] > 0);
    REQUIRE(data.count[Index(Zone::WriterWait)] == 1);
    REQUIRE(data.exclusive_ns[Index(Zone::WriterWait)] > 0);
}

TEST_CASE("Nx2Profile waits: the idle hook is not idle time", "[video_core]") {
    const Flags flags{false, true};
    const auto data = OnThread([] {
        const Nx2Profile::Scope wait{Zone::SplitWait};
        Sleep(1);
        const Nx2Profile::Scope hook{Zone::IdleHook};
        Sleep(20);
    });
    REQUIRE(data.count[Index(Zone::SplitWait)] == 1);
    REQUIRE(data.count[Index(Zone::IdleHook)] == 1);
    // The 20 ms hook is in its own zone, not in the 1 ms wait around it.
    REQUIRE(data.exclusive_ns[Index(Zone::IdleHook)] > data.exclusive_ns[Index(Zone::SplitWait)]);
}

TEST_CASE("Nx2Profile waits: with the profile and waits off nothing records", "[video_core]") {
    const Flags flags{false, false};
    const auto data = OnThread([] {
        const Nx2Profile::Scope push{Zone::Push};
        const Nx2Profile::Scope drain{Zone::SplitDrain};
        const Nx2Profile::Scope hook{Zone::IdleHook};
        Nx2Profile::AddDrain(DrainSite::Acquire, Nx2Profile::Ticks());
        Sleep(1);
    });
    for (size_t i = 0; i < data.count.size(); ++i) {
        REQUIRE(data.count[i] == 0);
        REQUIRE(data.exclusive_ns[i] == 0);
    }
    REQUIRE(data.drain_count[static_cast<size_t>(DrainSite::Acquire)] == 0);
}

TEST_CASE("Nx2Profile waits: the full profile still times every zone", "[video_core]") {
    const Flags flags{true, false};
    const auto data = OnThread([] {
        const Nx2Profile::Scope push{Zone::Push};
        const Nx2Profile::Scope drain{Zone::SplitDrain};
        Sleep(1);
    });
    REQUIRE(data.count[Index(Zone::Push)] == 1);
    REQUIRE(data.count[Index(Zone::SplitDrain)] == 1);
}

TEST_CASE("Nx2Profile waits: the front line", "[video_core]") {
    Nx2Profile::ThreadData data{};
    // 1 tick = 1 us: a 10 s window is 10,000,000 ticks.
    data.exclusive_ns[Index(Zone::SplitDrain)] = 3'000'000;
    data.count[Index(Zone::SplitDrain)] = 20'000;
    data.exclusive_ns[Index(Zone::WriterWait)] = 1'500'000;
    data.count[Index(Zone::WriterWait)] = 5'000;
    data.exclusive_ns[Index(Zone::PendingSleep)] = 2'000'000;
    data.count[Index(Zone::PendingSleep)] = 3'300;
    data.exclusive_ns[Index(Zone::Idle)] = 1'000'000;
    data.count[Index(Zone::Idle)] = 1'000;
    data.exclusive_ns[Index(Zone::Push)] = 999; // Work zones never count as waits.
    data.drain_count[static_cast<size_t>(DrainSite::Acquire)] = 15'000;
    data.drain_ns[static_cast<size_t>(DrainSite::Acquire)] = 2'500'000;
    data.drain_count[static_cast<size_t>(DrainSite::ListWriters)] = 5'000;
    data.drain_ns[static_cast<size_t>(DrainSite::ListWriters)] = 1'500'000;
    const std::string line = Nx2Profile::FormatWaitsLine(
        data, Nx2Profile::WaitsLine::Front, 10'000'000'000ULL, 10'000'000, 6'070'000'000ULL);
    REQUIRE(line ==
            "GPU waits per second: work 250.0 ms | wait back 450.0 ms (drain 300.0 ms 2000/s, "
            "writer 150.0 ms 500/s) | wait releases 200.0 ms (330/s) | wait guest 100.0 ms (100/s) "
            "| wait list copies 0.0 ms (0/s) | wait GPU 0.0 ms (0/s) | wait other 0.0 ms (0/s) | "
            "host_cpu 607.0 ms | window 10000.0 ms | drains 2000/s: acquire 1500/s 250.0 ms, list "
            "writers 500/s 150.0 ms");
    const std::string none = Nx2Profile::FormatWaitsLine(
        Nx2Profile::ThreadData{}, Nx2Profile::WaitsLine::Front, 10'000'000'000ULL, 10'000'000,
        std::nullopt);
    REQUIRE(none ==
            "GPU waits per second: work 1000.0 ms | wait back 0.0 ms (drain 0.0 ms 0/s, writer "
            "0.0 ms 0/s) | wait releases 0.0 ms (0/s) | wait guest 0.0 ms (0/s) | wait list "
            "copies 0.0 ms (0/s) | wait GPU 0.0 ms (0/s) | wait other 0.0 ms (0/s) | host_cpu "
            "unavailable | window 10000.0 ms | drains 0/s: none");
}

TEST_CASE("Nx2Profile waits: the back line", "[video_core]") {
    Nx2Profile::ThreadData data{};
    data.exclusive_ns[Index(Zone::SplitWait)] = 1'400'000;
    data.count[Index(Zone::SplitWait)] = 90'000;
    data.exclusive_ns[Index(Zone::IdleHook)] = 120'000;
    data.count[Index(Zone::IdleHook)] = 26'000;
    data.exclusive_ns[Index(Zone::PresentWait)] = 30'000;
    data.count[Index(Zone::PresentWait)] = 600;
    data.exclusive_ns[Index(Zone::GpuWait)] = 40'000;
    data.count[Index(Zone::GpuWait)] = 80;
    data.exclusive_ns[Index(Zone::ListGpuWait)] = 10'000;
    data.count[Index(Zone::ListGpuWait)] = 20;
    const std::string line = Nx2Profile::FormatWaitsLine(
        data, Nx2Profile::WaitsLine::Back, 10'000'000'000ULL, 10'000'000, 9'000'000'000ULL);
    REQUIRE(line == "GPU back waits per second: busy 852.0 ms | idle 140.0 ms (9000/s) | idle "
                    "hook 12.0 ms (2600/s) | present wait 3.0 ms (60/s) | GPU wait 5.0 ms (10/s) "
                    "| pipeline wait 0.0 ms (0/s) | wait other 0.0 ms (0/s) | host_cpu 900.0 ms | "
                    "window 10000.0 ms");
}
