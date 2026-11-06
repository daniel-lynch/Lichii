// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <string_view>

#include "common/common_types.h"

namespace VideoCommon::QueryWait {

using Milliseconds = std::chrono::milliseconds;

constexpr Milliseconds DefaultTimeout{2000};
/// Larger values are clamped, so the deadline arithmetic in wait_for cannot overflow.
constexpr Milliseconds MaxTimeout{3'600'000};

/// Parses an EDEN_NX2_QUERY_WAIT_MS value. Unset, empty or malformed keeps the default; 0 waits
/// without a bound (the #215 behaviour).
inline Milliseconds ParseTimeout(const char* value) {
    if (value == nullptr) {
        return DefaultTimeout;
    }
    const std::string_view text{value};
    u64 ms{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), ms);
    if (!text.empty() && error == std::errc::result_out_of_range &&
        end == text.data() + text.size()) {
        return MaxTimeout;
    }
    if (text.empty() || error != std::errc{} || end != text.data() + text.size()) {
        return DefaultTimeout;
    }
    if (ms > static_cast<u64>(MaxTimeout.count())) {
        return MaxTimeout;
    }
    return Milliseconds{static_cast<Milliseconds::rep>(ms)};
}

/// EDEN_NX2_QUERY_WAIT_MS: how long a macro DMA read of pending reports waits for their host
/// results before it reads guest memory as it is (default 2000; 0 = no bound). Read once.
inline Milliseconds Timeout() {
    static const Milliseconds timeout = ParseTimeout(std::getenv("EDEN_NX2_QUERY_WAIT_MS"));
    return timeout;
}

/// A one-shot flag set by an operation queued on another thread. Hold it in a shared_ptr and
/// capture that by value: the operation may run (or be destroyed unrun) after the waiter gave up.
class Completion {
public:
    void Signal() {
        {
            std::scoped_lock lock{mutex};
            done = true;
        }
        cv.notify_all();
    }

    /// Returns whether it was signalled. A zero timeout waits without a bound, unless
    /// `zero_unbounded` is false, when it only checks.
    bool WaitFor(Milliseconds timeout, bool zero_unbounded = true) {
        std::unique_lock lock{mutex};
        if (timeout.count() == 0) {
            if (zero_unbounded) {
                cv.wait(lock, [this] { return done; });
            }
            return done;
        }
        return cv.wait_for(lock, timeout, [this] { return done; });
    }

private:
    std::mutex mutex;
    std::condition_variable cv;
    bool done{};
};

/// How long one slice of WaitSliced blocks before it runs its slice callback.
constexpr Milliseconds Slice{5};

/// Waits for `completion` in `slice`-long slices, calling `on_slice` between them (on the split's
/// back: submit deferred work, which the fencing thread may be waiting for while this thread
/// blocks). Gives up after `timeout`; a zero timeout waits without a bound. True if signalled.
template <typename OnSlice>
bool WaitSliced(Completion& completion, Milliseconds timeout, Milliseconds slice,
                OnSlice&& on_slice) {
    using Clock = std::chrono::steady_clock;
    const bool bounded = timeout.count() != 0;
    const auto deadline = Clock::now() + timeout;
    while (true) {
        auto wait = slice;
        if (bounded) {
            const auto left = std::chrono::ceil<Milliseconds>(deadline - Clock::now());
            if (left.count() <= 0) {
                return completion.WaitFor(Milliseconds{0}, false);
            }
            wait = (std::min)(wait, left);
        }
        if (completion.WaitFor(wait, false)) {
            return true;
        }
        on_slice();
    }
}

/// Timeouts are logged on the first and then every this many.
constexpr u64 LogEvery = 100;

/// Counts a timed-out wait. Returns the running count, and whether this one should be logged.
struct TimeoutNote {
    u64 count;
    bool log;
};

inline TimeoutNote NoteTimeout(std::atomic<u64>& counter) {
    const u64 count = counter.fetch_add(1, std::memory_order_relaxed) + 1;
    return {count, (count - 1) % LogEvery == 0};
}

} // namespace VideoCommon::QueryWait
