// SPDX-License-Identifier: GPL-3.0-or-later

#include <bit>
#include <tuple>
#include <utility>
#include <catch2/catch_test_macros.hpp>

#include "video_core/buffer_cache/buffer_cache_base.h"
#include "video_core/buffer_cache/storage_buffer_size.h"

TEST_CASE("Buffer cache holds every shader storage binding", "[video_core][storage_bindings]") {
    using Channel = VideoCommon::BufferCacheChannelInfo;
    using ComputeBindings = decltype(std::declval<Channel>().compute_storage_buffers);
    using GraphicsBindings = decltype(std::declval<Channel>().storage_buffers)::value_type;
    STATIC_REQUIRE(std::tuple_size_v<ComputeBindings> == Shader::Info::MAX_SSBOS);
    STATIC_REQUIRE(std::tuple_size_v<GraphicsBindings> == Shader::Info::MAX_SSBOS);
    STATIC_REQUIRE(Shader::Info::MAX_SSBOS <= 64);
    STATIC_REQUIRE(sizeof(decltype(Channel::enabled_compute_storage_buffers)) * 8 >= Shader::Info::MAX_SSBOS);
    STATIC_REQUIRE(sizeof(decltype(Channel::written_compute_storage_buffers)) * 8 >= Shader::Info::MAX_SSBOS);
    STATIC_REQUIRE(sizeof(decltype(Channel::native_compute_storage_buffers)) * 8 >= Shader::Info::MAX_SSBOS);
    STATIC_REQUIRE(sizeof(decltype(Channel::bounded_compute_storage_buffers)) * 8 >= Shader::Info::MAX_SSBOS);
    STATIC_REQUIRE(sizeof(typename decltype(Channel::enabled_storage_buffers)::value_type) * 8 >= Shader::Info::MAX_SSBOS);
    STATIC_REQUIRE(sizeof(typename decltype(Channel::written_storage_buffers)::value_type) * 8 >= Shader::Info::MAX_SSBOS);


    ComputeBindings compute{};
    GraphicsBindings graphics{};
    u64 enabled{};
    for (u32 index = 0; index < Shader::Info::MAX_SSBOS; ++index) {
        compute.at(index).size = index + 1;
        graphics.at(index).size = index + 1;
        enabled |= u64{1} << index;
    }
    // Bananza's 18-buffer compute shader previously lost slots 16 and 17,
    // shifting the image descriptors that followed them.
    REQUIRE(compute.at(16).size == 17);
    REQUIRE(compute.at(17).size == 18);
    u32 count{};
    while (enabled != 0) {
        const auto index = static_cast<u32>(std::countr_zero(enabled));
        REQUIRE(compute.at(index).size == index + 1);
        REQUIRE(graphics.at(index).size == index + 1);
        enabled &= enabled - 1;
        ++count;
    }
    REQUIRE(count == Shader::Info::MAX_SSBOS);
}

TEST_CASE("Storage buffer size limits cap the kind extent before narrowing",
          "[video_core][storage_bindings]") {
    using VideoCommon::StorageBufferSizeLimit;
    constexpr u64 mib = 1024 * 1024;
    constexpr u64 four_gib = 1ULL << 32;
    // (kind_extent, next_qword, is_written, native_pointer, table_pointer)
    // Small extents are unchanged by the caps.
    CHECK(StorageBufferSizeLimit(4 * mib, 0x10a, false, true, false) == 4 * mib);
    CHECK(StorageBufferSizeLimit(16 * mib, 0x10a, false, true, false) == 8 * mib);
    CHECK(StorageBufferSizeLimit(80 * mib, 0, false, false, true) == 64 * mib);
    // NVN pointers take the next qword as their size when it fits the extent.
    CHECK(StorageBufferSizeLimit(4 * mib, 0x10a, false, false, false) == 0x10a);
    CHECK(StorageBufferSizeLimit(4 * mib, 0x10000010aULL, false, false, false) == 4 * mib);
    CHECK(StorageBufferSizeLimit(4 * mib, 8 * mib, false, false, false) == 4 * mib);
    // A kind range of 4 GiB or more must not wrap: exactly 4 GiB used to become a NULL binding
    // and 4 GiB + x a binding of x bytes.
    CHECK(StorageBufferSizeLimit(four_gib, 0, false, true, false) == 8 * mib);
    CHECK(StorageBufferSizeLimit(four_gib + 0x2000, 0, false, true, false) == 8 * mib);
    CHECK(StorageBufferSizeLimit(four_gib, 0, false, false, true) == 64 * mib);
    CHECK(StorageBufferSizeLimit(four_gib + 0x2000, 0, false, false, true) == 64 * mib);
    CHECK(StorageBufferSizeLimit(four_gib, 0, false, false, false) == 8 * mib);
    CHECK(StorageBufferSizeLimit(four_gib + 0x2000, 0x10a, true, false, false) == 0x10a);
    CHECK(StorageBufferSizeLimit(0, 0x10a, false, true, false) == 0);
}

TEST_CASE("Read-only table-pointer pools reach as far as a storage buffer binding",
          "[video_core][storage_bindings]") {
    using VideoCommon::StorageBufferSizeLimit;
    using VideoCommon::TablePoolReach;
    constexpr u64 kib = 1024;
    constexpr u64 mib = 1024 * kib;
    constexpr u64 four_gib = 1ULL << 32;
    // EDEN_NX2_TABLE_POOL_FULL=0 keeps the old 64 MiB cap whatever the device allows.
    CHECK(TablePoolReach(four_gib - 1, false) == 64 * mib);
    // Otherwise the device range, at most 1 GiB, less 64 KiB.
    CHECK(TablePoolReach(four_gib - 1, true) == 1024 * mib - 64 * kib);
    CHECK(TablePoolReach(128 * mib, true) == 128 * mib - 64 * kib);
    CHECK(TablePoolReach(32 * kib, true) == 0);
    // Ocarina of Time's light probes lie about 130 MiB past their pool pointer.
    const u64 reach = TablePoolReach(four_gib - 1, true);
    CHECK(StorageBufferSizeLimit(200 * mib, 0, false, false, true, reach) == 200 * mib);
    CHECK(StorageBufferSizeLimit(four_gib + 0x2000, 0, false, false, true, reach) == reach);
    // Written pools and other pointers keep their caps.
    CHECK(StorageBufferSizeLimit(200 * mib, 0, true, false, true, reach) == 8 * mib);
    CHECK(StorageBufferSizeLimit(200 * mib, 0, false, true, false, reach) == 8 * mib);
    CHECK(StorageBufferSizeLimit(200 * mib, 0x10a, false, false, false, reach) == 0x10a);
}
