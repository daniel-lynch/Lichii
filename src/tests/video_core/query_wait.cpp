// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "video_core/query_wait.h"

using namespace VideoCommon::QueryWait;
using namespace std::chrono_literals;

TEST_CASE("Query wait timeout parses EDEN_NX2_QUERY_WAIT_MS", "[video_core][nx2][query_wait]") {
    REQUIRE(ParseTimeout(nullptr) == DefaultTimeout);
    REQUIRE(DefaultTimeout == 2000ms);
    REQUIRE(ParseTimeout("") == DefaultTimeout);
    REQUIRE(ParseTimeout("abc") == DefaultTimeout);
    REQUIRE(ParseTimeout("-5") == DefaultTimeout);
    REQUIRE(ParseTimeout("250ms") == DefaultTimeout);
    REQUIRE(ParseTimeout("250") == 250ms);
    REQUIRE(ParseTimeout("0") == 0ms);
    REQUIRE(ParseTimeout("99999999999") == MaxTimeout);
    REQUIRE(ParseTimeout("999999999999999999999999") == MaxTimeout);
}

TEST_CASE("Query wait returns once signalled", "[video_core][nx2][query_wait]") {
    const auto completion = std::make_shared<Completion>();
    std::thread signaller{[completion] {
        std::this_thread::sleep_for(10ms);
        completion->Signal();
    }};
    REQUIRE(completion->WaitFor(5000ms));
    signaller.join();
    // Already signalled: no wait at all, bounded or not.
    REQUIRE(completion->WaitFor(1ms));
    REQUIRE(completion->WaitFor(0ms));
}

TEST_CASE("Query wait gives up when the operation never runs", "[video_core][nx2][query_wait]") {
    // The queued operation owns the state, as in RasterizerVulkan::FlushQueryResults.
    std::vector<std::function<void()>> queue;
    std::weak_ptr<Completion> watch;
    {
        const auto completion = std::make_shared<Completion>();
        watch = completion;
        queue.emplace_back([completion] { completion->Signal(); });
        const auto start = std::chrono::steady_clock::now();
        REQUIRE_FALSE(completion->WaitFor(30ms));
        REQUIRE(std::chrono::steady_clock::now() - start >= 30ms);
    }
    // The waiter is gone; the operation still holds valid state and can run late...
    REQUIRE_FALSE(watch.expired());
    queue.front()();
    REQUIRE(watch.lock()->WaitFor(1ms));
    // ...or be destroyed unrun (device loss, shutdown), which releases it.
    queue.clear();
    REQUIRE(watch.expired());
}

TEST_CASE("Sliced query wait runs its callback between slices and gives up at the timeout",
          "[video_core][nx2][query_wait]") {
    Completion completion;
    int slices{};
    const auto start = std::chrono::steady_clock::now();
    REQUIRE_FALSE(WaitSliced(completion, 40ms, 5ms, [&slices] { ++slices; }));
    const auto elapsed = std::chrono::steady_clock::now() - start;
    REQUIRE(elapsed >= 40ms);
    REQUIRE(elapsed < 2000ms);
    REQUIRE(slices >= 1);
    REQUIRE(slices <= 8);
}

TEST_CASE("Sliced query wait: the slice callback can unblock the operation",
          "[video_core][nx2][query_wait]") {
    // The deadlock it breaks: the operation runs only once this thread submits deferred work.
    for (const auto timeout : {2000ms, 0ms}) {
        const auto completion = std::make_shared<Completion>();
        std::atomic<bool> submitted{};
        std::thread fencer{[completion, &submitted] {
            while (!submitted.load()) {
                std::this_thread::yield();
            }
            completion->Signal();
        }};
        int slices{};
        REQUIRE(WaitSliced(*completion, timeout, 5ms, [&] {
            ++slices;
            submitted = true;
        }));
        fencer.join();
        REQUIRE(slices >= 1);
    }
}

TEST_CASE("Sliced query wait returns at once when already signalled",
          "[video_core][nx2][query_wait]") {
    Completion completion;
    completion.Signal();
    int slices{};
    REQUIRE(WaitSliced(completion, 2000ms, 5ms, [&slices] { ++slices; }));
    REQUIRE(slices == 0);
}

TEST_CASE("Query wait timeouts are logged on the first and every LogEvery-th",
          "[video_core][nx2][query_wait]") {
    std::atomic<u64> counter{};
    std::vector<u64> logged;
    for (u64 i = 0; i < 2 * LogEvery + 1; ++i) {
        const auto note = NoteTimeout(counter);
        REQUIRE(note.count == i + 1);
        if (note.log) {
            logged.push_back(note.count);
        }
    }
    REQUIRE(logged == std::vector<u64>{1, LogEvery + 1, 2 * LogEvery + 1});
}
