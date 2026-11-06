// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include "video_core/offline_compute_producer.h"
#include "video_core/offline_shader_producer.h"
#include "video_core/shader_archive_environment.h"
#include "video_core/shader_translation_cache.h"

namespace {
using VideoCommon::ArchiveEnvironment;
using namespace VideoCommon::TranslationCache;
using Catch::Matchers::ContainsSubstring;

// A native compute program as the archives hold it: bare instructions (no 128-byte header),
// then bank-1 constants; the control section carries the dispatch state.
struct SyntheticCompute {
    std::vector<u8> bytes = std::vector<u8>(0x200);
    std::vector<u8> control = std::vector<u8>(0xe0);

    static void Put(std::vector<u8>& out, size_t offset, u32 word) {
        for (size_t i = 0; i < 4; ++i) {
            out.at(offset + i) = static_cast<u8>(word >> (i * 8));
        }
    }
    SyntheticCompute() {
        Put(bytes, 0, 0x0000794d); // EXIT PT.
        Put(bytes, 8, 0x03800000);
        Put(bytes, 0x100, 0x12345678);
        Put(control, 0, 0x98761234);
        Put(control, 4, 0xc9);
        Put(control, 8, 20);
        Put(control, 0x38, 0x10);
        Put(control, 0x3c, 4);
        Put(control, 0x40, 0x100);
        control[0x56] = 5;
        Put(control, 0x58, 64);
        Put(control, 0x5c, 2);
        Put(control, 0x60, 1);
        Put(control, 0x64, 0x1f80);
        Put(control, 0x68, 16);
    }
    ArchiveEnvironment Make() const {
        return ArchiveEnvironment(ArchiveEnvironment::ComputeProgram{}, bytes, control);
    }
};

Shader::Profile TestProfile() {
    Shader::Profile profile{};
    profile.supported_spirv = 0x00010300;
    return profile;
}
} // namespace

TEST_CASE("Compute control sections decode the QMD dispatch state", "[video_core][archive_environment]") {
    SyntheticCompute source;
    const auto state = ArchiveEnvironment::ReadComputeState(source.control);
    REQUIRE(state.workgroup_size == std::array<u32, 3>{64, 2, 1});
    // The launch descriptor allocates shared memory in 256-byte units.
    REQUIRE(state.shared_memory_size == 0x2000);
    REQUIRE(state.local_memory_size == 16);
    SyntheticCompute::Put(source.control, 0x64, 0x80);
    REQUIRE(ArchiveEnvironment::ReadComputeState(source.control).shared_memory_size == 0x100);
    SyntheticCompute::Put(source.control, 0x64, 0x4800);
    REQUIRE(ArchiveEnvironment::ReadComputeState(source.control).shared_memory_size == 0x4800);
    SyntheticCompute::Put(source.control, 0x64, 0);
    REQUIRE(ArchiveEnvironment::ReadComputeState(source.control).shared_memory_size == 0);

    SECTION("short control") { source.control.resize(0x68); }
    SECTION("unknown control version") { SyntheticCompute::Put(source.control, 8, 21); }
    SECTION("zero dimension") { SyntheticCompute::Put(source.control, 0x5c, 0); }
    SECTION("too many invocations") { SyntheticCompute::Put(source.control, 0x5c, 32); }
    SECTION("absurd shared memory") { SyntheticCompute::Put(source.control, 0x64, 0x100000); }
    REQUIRE_THROWS(ArchiveEnvironment::ReadComputeState(source.control));
}

TEST_CASE("Archive compute environment matches the runtime's native compute environment",
          "[video_core][archive_environment]") {
    auto env = SyntheticCompute{}.Make();
    REQUIRE(env.ShaderStage() == Shader::Stage::Compute);
    REQUIRE(env.Analyze().has_value());
    REQUIRE(env.IsNativeProgram());
    REQUIRE(env.HasNativeInstructions());
    REQUIRE(env.IsProprietaryDriver());
    // ComputeEnvironment in native mode: past an all-zero header, never one read from memory.
    REQUIRE(env.StartAddress() == sizeof(Shader::ProgramHeader));
    const Shader::ProgramHeader zero{};
    REQUIRE(std::memcmp(&env.SPH(), &zero, sizeof(zero)) == 0);
    REQUIRE(env.WorkgroupSize() == std::array<u32, 3>{64, 2, 1});
    REQUIRE(env.SharedMemorySize() == 0x2000);
    REQUIRE(env.LocalMemorySize() == 16);
    REQUIRE(env.TextureBoundBuffer() == 0);
    REQUIRE_FALSE(env.HasHLEMacroState());
    REQUIRE(env.ReadCbufValue(1, 0) == 0x12345678);
    REQUIRE_THROWS_WITH(env.ReadCbufValue(0, 0), ContainsSubstring("missing_constant"));
}

TEST_CASE("Archive compute environment rejects malformed programs", "[video_core][archive_environment]") {
    SyntheticCompute source;
    SECTION("graphics control stage") { source.control[0x56] = 1; }
    SECTION("code past the program") { SyntheticCompute::Put(source.control, 0x38, 0x400); }
    SECTION("unaligned code") { SyntheticCompute::Put(source.control, 0x38, 0x18); }
    SECTION("constant section overlaps code") { SyntheticCompute::Put(source.control, 0x40, 0x8); }
    SECTION("incomplete capture") { SyntheticCompute::Put(source.bytes, 0, 0x00007918); }
    SECTION("trap guard") { SyntheticCompute::Put(source.bytes, 0, 0x0000795c); }
    REQUIRE_THROWS(source.Make());
    // The graphics constructor still refuses compute.
    REQUIRE_THROWS_WITH(ArchiveEnvironment(SyntheticCompute{}.bytes, SyntheticCompute{}.control,
                                           Shader::Stage::Compute),
                        ContainsSubstring("unsupported_stage"));
}

TEST_CASE("The compute stage key covers the dispatch state", "[video_core][archive_environment]") {
    const Digest config = MakeConfigDigest(TestProfile(), {}, 0);
    SyntheticCompute source;
    auto base_env = source.Make();
    const auto hash = *base_env.Analyze();
    const auto base = MakeStageKey(base_env, hash, 5, config);
    // Slot 5 is the runtime's compute slot, not stage + 1.
    REQUIRE(MakeStageKey(base_env, hash, 6, config) != base);
    for (const size_t offset : std::array<size_t, 4>{0x58, 0x5c, 0x64, 0x68}) {
        auto changed = source;
        SyntheticCompute::Put(changed.control, offset, offset == 0x64 ? 0x2100 : 4);
        auto env = changed.Make();
        REQUIRE(*env.Analyze() == hash);
        REQUIRE(MakeStageKey(env, hash, 5, config) != base);
    }
    // Shared sizes within one 256-byte unit are the same launch.
    auto rounded = source;
    SyntheticCompute::Put(rounded.control, 0x64, 0x1f01);
    auto env = rounded.Make();
    REQUIRE(MakeStageKey(env, hash, 5, config) == base);
}

TEST_CASE("Offline producer emits compute modules under the runtime's keys and resumes them",
          "[video_core][translation_cache][archive_environment]") {
    namespace Offline = VideoCommon::OfflineShaders;
    REQUIRE(Offline::ComputeEnabled());
    const auto directory = std::filesystem::temp_directory_path() / "lichii_offline_compute_test";
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    SyntheticCompute source;
    auto write = [&](const char* name, const std::vector<u8>& bytes) {
        std::ofstream file(directory / name, std::ios::binary);
        file.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    };
    write("code.bin", source.bytes);
    write("control.bin", source.control);
    u64 fingerprint = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < 256; ++i) fingerprint = (fingerprint ^ source.bytes[i]) * 0x100000001b3ULL;
    nlohmann::json row{{"container", "code.bin"}, {"control", "control.bin"},
                       {"offset", 0}, {"size", source.bytes.size()}, {"control_offset", 0},
                       {"constants_offset", 256}, {"constants_size", 4},
                       {"stage", "compute"}, {"fingerprint", fmt::format("{:016x}", fingerprint)},
                       {"bank1_words", {0x12345678}}};
    // A duplicate row (the same program in two containers) produces one module.
    nlohmann::json index{{"schema_version", 1}, {"title_id", "0400000000000000"},
                         {"programs", {row, row}}};
    const auto input = directory / "index.json";
    {
        std::ofstream file(input);
        file << index;
    }
    const auto path = directory / "cache.bin";
    const auto profile = TestProfile();
    const auto config = MakeConfigDigest(profile, {}, 0);
    auto env = source.Make();
    const auto stage_key = MakeStageKey(env, *env.Analyze(), 5, config);
    {
        Store store;
        store.Open(path, config);
        const auto first = Offline::Run(input, 0x0400000000000000ULL, 2, {}, profile, {}, config, store);
        REQUIRE(first.skipped.empty());
        REQUIRE(first.compute.translated == 1);
        REQUIRE(first.compute.resumed == 1);
        REQUIRE(first.compute.modules == 1);
        REQUIRE(first.modules == 1);
        REQUIRE(first.compute.module_bytes == first.module_bytes);
        REQUIRE(first.product_modules == 1);
    }
    {
        // What CreateComputePipeline does at the first dispatch: the stage key with slot 5,
        // replay against the live environment, then the zero-digest, default-binding variant.
        Store store;
        store.Open(path, config);
        auto live = source.Make();
        const auto entry = store.FindEntry(stage_key, live);
        REQUIRE(entry);
        REQUIRE(entry->offline);
        REQUIRE(entry->meta.stage == Shader::Stage::Compute);
        REQUIRE(entry->meta.workgroup_size == std::array<u32, 3>{64, 2, 1});
        REQUIRE(entry->meta.shared_memory_size == 0x2000);
        REQUIRE(entry->meta.local_memory_size == 16);
        const auto variant =
            store.FindVariant(MakeVariantKey(entry->id, Digest{}, Shader::Backend::Bindings{}));
        REQUIRE(variant.has_value());
        REQUIRE(variant->spirv.front() == 0x07230203);
        // A rerun translates nothing.
        const auto second = Offline::Run(input, 0x0400000000000000ULL, 2, {}, profile, {}, config, store);
        REQUIRE(second.translated == 0);
        REQUIRE(second.modules == 0);
        REQUIRE(second.compute.resumed == 2);
        REQUIRE(second.validated_modules >= 1);
        REQUIRE(second.product_modules == 1);
    }
    std::filesystem::remove_all(directory);
}

TEST_CASE("Compute rows parse with bank-1 checks", "[video_core][translation_cache]") {
    namespace Offline = VideoCommon::OfflineShaders;
    nlohmann::json index{{"schema_version", 1}, {"title_id", "0400000000000000"},
        {"programs", {{{"container", "code.bin"}, {"control", "control.bin"},
                       {"offset", 0}, {"size", 512}, {"control_offset", 0},
                       {"constants_offset", 256}, {"constants_size", 4},
                       {"fingerprint", "123456789abcdef0"}, {"stage", "compute"}}}}};
    const auto parsed = Offline::ParseInput(index.dump(), {});
    REQUIRE(parsed.programs.at(0).stage == Shader::Stage::Compute);
    REQUIRE(parsed.programs.at(0).skip_reason.empty());
    index["programs"][0]["constants_size"] = Offline::MaxBank1Bytes + 4;
    index["programs"][0]["constants_offset"] = 0;
    REQUIRE(Offline::ParseInput(index.dump(), {}).programs.at(0).skip_reason == "bank1_too_large");
    index["programs"][0]["constants_offset"] = 400;
    REQUIRE_THROWS(Offline::ParseInput(index.dump(), {}));
}
