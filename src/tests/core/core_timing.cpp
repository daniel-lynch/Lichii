// SPDX-FileCopyrightText: 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "core/core.h"
#include "core/core_timing.h"

namespace {
// Numbers are chosen randomly to make sure the correct one is given.
constexpr std::array<u64, 5> calls_order{{2, 0, 1, 4, 3}};
std::array<s64, 5> delays{};
std::bitset<5> callbacks_ran_flags;
u64 expected_callback = 0;

template <unsigned int IDX>
std::optional<std::chrono::nanoseconds> HostCallbackTemplate(s64 time,
                                                             std::chrono::nanoseconds ns_late) {
    static_assert(IDX < callbacks_ran_flags.size(), "IDX out of range");
    callbacks_ran_flags.set(IDX);
    delays[IDX] = ns_late.count();
    ++expected_callback;
    return std::nullopt;
}

struct ScopeInit final {
    ScopeInit() {
        callbacks_ran_flags.reset();
        delays.fill(0);
        expected_callback = 0;
        core_timing.SetMulticore(true);
        core_timing.Initialize([]() {});
    }

    Core::Timing::CoreTiming core_timing;
};

u64 TestTimerSpeed(Core::Timing::CoreTiming& core_timing) {
    const u64 start = core_timing.GetGlobalTimeNs().count();
    volatile u64 placebo = 0;
    for (std::size_t i = 0; i < 1000; i++) {
        placebo = placebo + core_timing.GetGlobalTimeNs().count();
    }
    const u64 end = core_timing.GetGlobalTimeNs().count();
    return end - start;
}

} // Anonymous namespace

TEST_CASE("CoreTiming[BasicOrder]", "[core]") {
    ScopeInit guard;
    auto& core_timing = guard.core_timing;
    std::vector<std::shared_ptr<Core::Timing::EventType>> events{
        Core::Timing::CreateEvent("callbackA", HostCallbackTemplate<0>),
        Core::Timing::CreateEvent("callbackB", HostCallbackTemplate<1>),
        Core::Timing::CreateEvent("callbackC", HostCallbackTemplate<2>),
        Core::Timing::CreateEvent("callbackD", HostCallbackTemplate<3>),
        Core::Timing::CreateEvent("callbackE", HostCallbackTemplate<4>),
    };

    expected_callback = 0;
    callbacks_ran_flags.reset();

    core_timing.SyncPause(true);

    const u64 one_micro = 1000U;
    for (std::size_t i = 0; i < events.size(); i++) {
        const u64 order = calls_order[i];
        const auto future_ns = std::chrono::nanoseconds{static_cast<s64>(i * one_micro + 100)};

        core_timing.ScheduleEvent(future_ns, events[order]);
    }
    /// test pause
    REQUIRE(callbacks_ran_flags.none());

    core_timing.Pause(false); // No need to sync

    while (core_timing.HasPendingEvents())
        ;

    REQUIRE(callbacks_ran_flags.all());

    for (std::size_t i = 0; i < delays.size(); i++) {
        const double delay = static_cast<double>(delays[i]);
        const double micro = delay / 1000.0f;
        const double mili = micro / 1000.0f;
        printf("HostTimer Pausing Delay[%zu]: %.3f %.6f\n", i, micro, mili);
    }
}

TEST_CASE("CoreTiming[BasicOrderNoPausing]", "[core]") {
    ScopeInit guard;
    auto& core_timing = guard.core_timing;
    std::vector<std::shared_ptr<Core::Timing::EventType>> events{
        Core::Timing::CreateEvent("callbackA", HostCallbackTemplate<0>),
        Core::Timing::CreateEvent("callbackB", HostCallbackTemplate<1>),
        Core::Timing::CreateEvent("callbackC", HostCallbackTemplate<2>),
        Core::Timing::CreateEvent("callbackD", HostCallbackTemplate<3>),
        Core::Timing::CreateEvent("callbackE", HostCallbackTemplate<4>),
    };

    core_timing.SyncPause(true);
    core_timing.SyncPause(false);

    expected_callback = 0;
    callbacks_ran_flags.reset();

    const u64 start = core_timing.GetGlobalTimeNs().count();
    const u64 one_micro = 1000U;

    for (std::size_t i = 0; i < events.size(); i++) {
        const u64 order = calls_order[i];
        const auto future_ns = std::chrono::nanoseconds{static_cast<s64>(i * one_micro + 100)};
        core_timing.ScheduleEvent(future_ns, events[order]);
    }

    const u64 end = core_timing.GetGlobalTimeNs().count();
    const double scheduling_time = static_cast<double>(end - start);
    const double timer_time = static_cast<double>(TestTimerSpeed(core_timing));

    while (core_timing.HasPendingEvents())
        ;

    REQUIRE(callbacks_ran_flags.all());

    for (std::size_t i = 0; i < delays.size(); i++) {
        const double delay = static_cast<double>(delays[i]);
        const double micro = delay / 1000.0f;
        const double mili = micro / 1000.0f;
        printf("HostTimer No Pausing Delay[%zu]: %.3f %.6f\n", i, micro, mili);
    }

    const double micro = scheduling_time / 1000.0f;
    const double mili = micro / 1000.0f;
    printf("HostTimer No Pausing Scheduling Time: %.3f %.6f\n", micro, mili);
    printf("HostTimer No Pausing Timer Time: %.3f %.6f\n", timer_time / 1000.f,
           timer_time / 1000000.f);
}

// The cheat engine's shutdown relies on this: a looping event (its frame callback reads services
// owned by System) is unscheduled with Wait, and only then are those services torn down. Wait must
// return only after a callback already running on the timer thread has finished, and no callback
// may start afterwards, or it touches a destroyed object (the shutdown SIGSEGV in
// ServiceManager::GetService from DmntCheatVm::Execute).
TEST_CASE("CoreTiming[UnscheduleWaitsForLoopingCallback]", "[core]") {
    ScopeInit guard;
    auto& core_timing = guard.core_timing;
    core_timing.SyncPause(true);
    core_timing.SyncPause(false);

    std::atomic_bool service_alive{true};
    std::atomic_bool in_callback{false};
    std::atomic_bool touched_dead_service{false};
    std::atomic<u32> calls{0};
    const auto event = Core::Timing::CreateEvent(
        "LoopingServiceReader",
        [&](s64, std::chrono::nanoseconds) -> std::optional<std::chrono::nanoseconds> {
            in_callback = true;
            ++calls;
            // Long enough that the unschedule below lands while this is still running.
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
            if (!service_alive) {
                touched_dead_service = true;
            }
            in_callback = false;
            return std::nullopt;
        });
    core_timing.ScheduleLoopingEvent(std::chrono::microseconds{100}, std::chrono::microseconds{100},
                                     event);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!in_callback && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    REQUIRE(in_callback);

    core_timing.UnscheduleEvent(event, Core::Timing::UnscheduleEventType::Wait);
    REQUIRE_FALSE(in_callback);
    const u32 calls_at_unschedule = calls;
    service_alive = false; // The service is torn down only after the unschedule returned.

    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    REQUIRE(calls == calls_at_unschedule);
    REQUIRE_FALSE(touched_dead_service);
}
