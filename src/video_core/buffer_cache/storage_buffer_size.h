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
[[nodiscard]] constexpr u32 StorageBufferSizeLimit(u64 kind_extent, u64 next_qword,
                                                   bool is_written, bool native_pointer,
                                                   bool table_pointer) {
    constexpr u64 mib = 1024 * 1024;
    // A pointer read from a table in memory is the base of an object pool, read anywhere in it
    // (Ocarina of Time's grass instances lie 12-18 MiB past theirs): bound to the end of its
    // mapping, up to 64 MiB. The 8 MiB cap made those reads return 0 and culled every blade.
    // Written pools keep the cap: a write marks the whole binding GPU-modified.
    if (table_pointer && !is_written) {
        return static_cast<u32>((std::min)(kind_extent, 64 * mib));
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

} // namespace VideoCommon
