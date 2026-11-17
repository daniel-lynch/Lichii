// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <optional>
#include <unordered_map>

#include "common/common_types.h"

namespace VideoCommon {

/// EDEN_NX2_ACQUIRE_INFLIGHT=1 (off by default, read when the GPU split starts): a semaphore
/// acquire passes on the last release the front sent to its word when that release's payload is
/// known, meets the acquire, and has not landed yet: still queued for the split's back, or run by
/// it as a report that lands once the host GPU passed its fence.
///
/// Why: fence-flagged reports and releases write memory only once the host GPU passed their fence
/// (FenceManager::SignalFence, g_signaling_query_report_fence). An acquire on one waits for the
/// host GPU to finish everything submitted so far while the front and the back both idle (C17.15:
/// ~1 ms a frame in the Market, ~2 ms in Hyrule Field). Host GPU work the acquiring channel sends
/// after the acquire goes to the same back and host queue after the release, so it runs after the
/// work the release follows.
///
/// What it does not order (review of 422cea3880), and how it is handled:
/// - Writes the back lands at once: a payload report or a compute/QMD read-modify-write release
///   over a slot with a report still waiting for its fence would overtake it. With the switch on
///   they wait in fence order instead (QueryCacheBase::CounterReport, InflightOrderedRelease).
/// - Equal and mask acquires pass only on a release the back already ran whose report is the
///   back's last write to the word with the same payload (InflightIssues), never on a queued one.
/// - Open (not handled): back-side reads of guest memory the report writes, before it lands
///   (buffer-cache uploads of report memory, the non-accelerated query condition), and guest CPU
///   reads ordered only by a later syncpoint or immediate release of the acquiring channel. Only
///   front macro DMA reads of report memory wait for settled reports. Default off for that reason.
inline bool g_acquire_inflight{};

/// Front thread: the last release the front sent to a semaphore word (GpuSplit::NoteSemaphoreWrite).
struct InflightNote {
    u32 payload;
    bool known;   ///< False: a reduction or counter report, whose value the front can't tell
    u64 position; ///< The split's write position when it was sent
};

class InflightNotes {
public:
    void Note(u64 device_address, u32 payload, bool known, u64 position) {
        if (notes.size() >= 8192) {
            notes.clear(); // A word not found decides as before.
        }
        notes.insert_or_assign(device_address & ~u64{3}, InflightNote{payload, known, position});
    }
    [[nodiscard]] std::optional<InflightNote> Find(u64 device_address) const {
        const auto it = notes.find(device_address & ~u64{3});
        return it == notes.end() ? std::nullopt : std::optional<InflightNote>{it->second};
    }
    void Forget(u64 device_address) {
        notes.erase(device_address & ~u64{3});
    }
    void Clear() {
        notes.clear();
    }

private:
    std::unordered_map<u64, InflightNote> notes;
};

inline InflightNotes& GetInflightNotes() {
    static InflightNotes notes;
    return notes;
}

/// Back thread: the last report write to each word the back ran (QueryCacheBase::CounterReport),
/// with its payload and the low 16 bits of its slot's async issue count after it: an async one
/// lands once the slot's completed count reaches that, one written at once is recorded with the
/// count as it stood (landed). Read-modify-write releases the query cache doesn't see (compute
/// and QMD reductions) Forget the word. Words share entries by hash; a 16-bit tag (never 0) keeps
/// one word's entry from answering for another, and a payload that differs from the front's note
/// means the entry is some other write.
struct InflightIssue {
    u32 payload;
    u16 issued_after; ///< Low 16 bits
};

class InflightIssues {
public:
    static constexpr u32 Entries = 1u << 14;

    void Wrote(u64 device_address, u32 payload, u32 issued_after) {
        entries[Index(device_address)].store((u64{Tag(device_address)} << 48) |
                                                 (u64{payload} << 16) | (issued_after & 0xffff),
                                             std::memory_order_release);
    }
    void Forget(u64 device_address) {
        entries[Index(device_address)].store(0, std::memory_order_release);
    }
    [[nodiscard]] std::optional<InflightIssue> Find(u64 device_address) const {
        const u64 entry = entries[Index(device_address)].load(std::memory_order_acquire);
        if (static_cast<u16>(entry >> 48) != Tag(device_address)) {
            return std::nullopt;
        }
        return InflightIssue{static_cast<u32>(entry >> 16), static_cast<u16>(entry)};
    }
    void Clear() {
        for (auto& entry : entries) {
            entry.store(0, std::memory_order_relaxed);
        }
    }

private:
    static u64 Hash(u64 device_address) {
        return (device_address >> 2) * 0x9E3779B97F4A7C15ULL;
    }
    static size_t Index(u64 device_address) {
        return static_cast<size_t>(Hash(device_address) >> 50) % Entries;
    }
    static u16 Tag(u64 device_address) {
        return static_cast<u16>(1 + (Hash(device_address) >> 16) % 0xffff);
    }
    std::array<std::atomic<u64>, Entries> entries{};
};

/// Whether a 16-bit issue count is still ahead of the slot's completed count.
[[nodiscard]] inline bool InflightUnlanded(u16 issued_after, u32 completed) {
    const u16 behind = static_cast<u16>(issued_after - static_cast<u16>(completed));
    return behind != 0 && behind < 0x8000;
}

inline InflightIssues& GetInflightIssues() {
    static InflightIssues issues;
    return issues;
}

enum class InflightStep {
    None,     ///< No release in flight meets it: decide as before
    Queued,   ///< Pass: the release meets it and the back has not run it yet
    Unlanded, ///< Pass: the back ran it as a report whose fence the host GPU has not passed
};

/// `note`: the last release the front sent to the word. `queued`: the back has not run it yet
/// (GpuSplit::QueuedSemaphoreWriteAt finds it at its position). `issue`: the last report write to
/// the word the back ran (InflightIssues), `completed`: the slot's completed count now.
/// - Queued: passes only a greater-or-equal acquire (`reaches`), as CheckWithoutDrain already did:
///   for an equal or mask one, writes queued between the release and the acquire can't be seen.
/// - Run: passes when the back's last write to the word carries the noted payload and has not
///   landed. Counts complete in fence order, so a report that completes out of order can only
///   make it look landed early, which decides as before.
template <typename Matches>
[[nodiscard]] InflightStep DecideInflight(const Matches& matches, bool reaches,
                                          const std::optional<InflightNote>& note, bool queued,
                                          const std::optional<InflightIssue>& issue, u32 completed) {
    if (!note || !note->known || !matches(note->payload)) {
        return InflightStep::None;
    }
    if (queued) {
        return reaches ? InflightStep::Queued : InflightStep::None;
    }
    if (!issue || issue->payload != note->payload || !InflightUnlanded(issue->issued_after, completed)) {
        // Landed, written at once, or the back's last write is another one: memory decides.
        return InflightStep::None;
    }
    return InflightStep::Unlanded;
}

} // namespace VideoCommon
