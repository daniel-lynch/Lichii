// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>

#include "common/common_types.h"

namespace VideoCommon {

/// EDEN_NX2_ACQUIRE_NODRAIN=1 (off by default): equal and mask semaphore acquires are decided
/// without draining the GPU split as greater-or-equal ones are (Puller CheckWithoutDrain), as of
/// the split's last start.
inline bool g_acquire_nodrain{};

/// What the front can decide about an equal or mask acquire without draining the split.
enum class EqualMaskStep {
    Met,           ///< The word in memory meets it, and nothing queued can still write the word
    NotMet,        ///< Poll again later (a latch, a store or the 1 ms valve ends the wait)
    WaitForWrites, ///< Wait for the queued work that can write the word, then read it again
    Drain,         ///< Drain the split and read the word as before
};

/// The last semaphore release the front sent to the word that the back may not have run yet.
struct QueuedWordRelease {
    u32 payload;
    bool known; ///< False: a reduction or counter report, whose value the front can't tell
};

/// Equal and mask words need not only advance, so unlike a greater-or-equal acquire a value that
/// meets one now may not after the queued work ran. Only the word in memory passes one, once
/// nothing queued can write it: a queued release that meets it is waited for, not taken on its
/// word as for greater-or-equal acquires. Most releases are fence-flagged and land only once the
/// host GPU passed them; passing on one before it landed (the first VERIFY runs: ~55 a second,
/// all "unlanded") let the channel run earlier than the drain it replaces ever did.
/// Not meeting is always safe to decide: the channel polls again, a back write that meets it
/// latches it (Nx2SemaphoreWaiters), a CPU store wakes the GPU thread (WordStored), and a waiting
/// acquire drains once per 1 ms for anything else.
/// `matches`: the acquire's predicate. `word`: the word as a drained read would see it now
/// (memory, or a mirrored value Nx2MirrorShadow holds). `writes_queued`: queued work that can
/// write the word (GpuSplit::HasQueuedWritesAt).
template <typename Matches>
[[nodiscard]] EqualMaskStep DecideEqualMask(const Matches& matches,
                                            const std::optional<QueuedWordRelease>& queued,
                                            u32 word, bool writes_queued) {
    if (queued) {
        if (!queued->known) {
            return EqualMaskStep::Drain;
        }
        if (!matches(queued->payload)) {
            // What the word will hold falls short; waiting never passes it early.
            return EqualMaskStep::NotMet;
        }
        return EqualMaskStep::WaitForWrites;
    }
    if (!matches(word)) {
        return EqualMaskStep::NotMet;
    }
    return writes_queued ? EqualMaskStep::WaitForWrites : EqualMaskStep::Met;
}

} // namespace VideoCommon
