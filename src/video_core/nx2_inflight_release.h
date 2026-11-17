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
/// ~1 ms a frame in the Market, ~2 ms in Hyrule Field). Everything the acquiring channel sends
/// after the acquire goes to the same back and the same host queue after the release, so the host
/// GPU runs it after the work the release follows, as for a channel that saw the word meet it.
/// Memory still gets the value when the fence passes, so what the guest CPU sees is unchanged.
/// Front-side reads after the acquire wait as for a queued release (GpuSplit::NoteSync), and
/// GPU-written command lists still wait for the host GPU (DmaPusher pending_list_tick).
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

/// Back thread: the last report issued to a word that lands later (an async report, counted in
/// Tegra::g_nx2_async_reports_issued), with its slot's issue count after it. It has landed once
/// the slot's completed count reached that count. Words share entries by hash; the tag keeps one
/// word's entry from answering for another.
class InflightIssues {
public:
    static constexpr u32 Entries = 1u << 14;

    void Issued(u64 device_address, u32 issued_after) {
        const u32 tag = static_cast<u32>(device_address >> 2);
        entries[Index(device_address)].store((u64{tag} << 32) | issued_after,
                                             std::memory_order_release);
    }
    [[nodiscard]] std::optional<u32> Find(u64 device_address) const {
        const u64 entry = entries[Index(device_address)].load(std::memory_order_acquire);
        if (static_cast<u32>(entry >> 32) != static_cast<u32>(device_address >> 2) ||
            static_cast<u32>(entry) == 0) {
            return std::nullopt;
        }
        return static_cast<u32>(entry);
    }
    void Clear() {
        for (auto& entry : entries) {
            entry.store(0, std::memory_order_relaxed);
        }
    }

private:
    static size_t Index(u64 device_address) {
        return static_cast<size_t>(((device_address >> 2) * 0x9E3779B97F4A7C15ULL) >> 50) %
               Entries;
    }
    std::array<std::atomic<u64>, Entries> entries{};
};

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
/// (GpuSplit::QueuedSemaphoreWriteAt finds it at its position). `issued_after`: the word's last
/// async report (InflightIssues), `completed`: the slot's completed count now. Counts complete in
/// fence order, so the report has landed once `completed` reached `issued_after`; a report that
/// completes out of order can only make it look landed early, which decides as before.
template <typename Matches>
[[nodiscard]] InflightStep DecideInflight(const Matches& matches,
                                          const std::optional<InflightNote>& note, bool queued,
                                          std::optional<u32> issued_after, u32 completed) {
    if (!note || !note->known || !matches(note->payload)) {
        return InflightStep::None;
    }
    if (queued) {
        return InflightStep::Queued;
    }
    if (!issued_after || static_cast<s32>(completed - *issued_after) >= 0) {
        // It wrote memory as the back ran it, or it landed: memory holds it, or whatever the guest
        // wrote since. Memory decides.
        return InflightStep::None;
    }
    return InflightStep::Unlanded;
}

} // namespace VideoCommon
