// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>

#include "common/common_types.h"

namespace VideoCommon {

/// Size limit of a storage buffer binding, before it is clipped to a continuous range.
/// `kind_extent` is the memory-layout (kind) extent at the pointer and may be 4 GiB or more, so
/// every cap is applied in u64 before narrowing: narrowing first made an exact 4 GiB extent a
/// zero-size (NULL) binding and 4 GiB + x a binding of x bytes.
/// `table_pool_reach` is how far a read-only table-pointer pool may be bound (TablePoolReach).
[[nodiscard]] constexpr u32 StorageBufferSizeLimit(u64 kind_extent, u64 next_qword,
                                                   bool is_written, bool native_pointer,
                                                   bool table_pointer,
                                                   u64 table_pool_reach = 64 * 1024 * 1024) {
    constexpr u64 mib = 1024 * 1024;
    // A pointer read from a table in memory is the base of an object pool, read anywhere in
    // it: bound to the end of its mapping, as far as a storage buffer may reach (at most
    // 1 GiB). Ocarina of Time's grass instances lie 12-18 MiB past theirs (an 8 MiB cap
    // culled every blade), and its light probe records reach about 130 MiB past theirs: a
    // 64 MiB cap left the far probes outside the binding, and the fog took one overbright
    // constant from them. Written pools keep a cap: a write marks the whole binding
    // GPU-modified.
    if (table_pointer && !is_written) {
        return static_cast<u32>((std::min)(kind_extent, table_pool_reach));
    }
    // A native (Switch 2) compute program's pointer is bare: the word after it is another
    // parameter (a dimension, stride or texture handle), not an NVN buffer size. Taking it as
    // one bound Fortune's Weave's DLSS input features to 266 bytes and dropped the rest.
    if (native_pointer) {
        return static_cast<u32>((std::min)(kind_extent, 8 * mib));
    }
    const u32 packed_size = static_cast<u32>(next_qword);
    if ((next_qword >> 32) == 0 && packed_size != 0 && packed_size <= kind_extent) {
        return packed_size;
    }
    return static_cast<u32>((std::min)(kind_extent, 8 * mib));
}

/// How far a read-only table-pointer pool is bound: the device's storage buffer range, at most
/// 1 GiB, less 64 KiB of headroom for the aligned-down offset. `full_pools` false keeps the old
/// 64 MiB cap (EDEN_NX2_TABLE_POOL_FULL=0).
[[nodiscard]] constexpr u64 TablePoolReach(u64 max_storage_range, bool full_pools) {
    constexpr u64 kib = 1024;
    constexpr u64 mib = 1024 * kib;
    if (!full_pools) {
        return 64 * mib;
    }
    const u64 reach = (std::min)(max_storage_range, 1024 * mib);
    return reach - (std::min)(reach, 64 * kib);
}

} // namespace VideoCommon
