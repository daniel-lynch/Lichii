// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <fstream>
#include <nlohmann/json.hpp>
#include <catch2/catch_test_macros.hpp>
#include "video_core/offline_shader_producer.h"

namespace {
using namespace VideoCommon::TranslationCache;
using namespace VideoCommon::OfflineShaders;
nlohmann::json Index() {
    return {{"schema_version", 1}, {"title_id", "0400000000000000"},
            {"programs", {{{"container", "code.bin"}, {"control", "control.bin"},
                           {"offset", 0}, {"size", 512}, {"control_offset", 0},
                           {"constants_offset", 256}, {"constants_size", 4},
                           {"fingerprint", "123456789abcdef0"}, {"stage", "vertex"}}}}};
}
}

TEST_CASE("Offline program index validates bounded ranges and metadata", "[video_core][translation_cache]") {
    auto input = Index();
    const auto parsed = ParseInput(input.dump(), "/tmp/offline");
    REQUIRE(parsed.title_id == 0x0400000000000000ULL);
    REQUIRE(parsed.programs.size() == 1);
    REQUIRE(parsed.programs[0].container == "/tmp/offline/code.bin");
    REQUIRE(parsed.programs[0].stage == Shader::Stage::VertexB);
    SECTION("negative offset") { input["programs"][0]["offset"] = -1; }
    SECTION("floating size") { input["programs"][0]["size"] = 512.0; }
    SECTION("allocation limit") { input["programs"][0]["size"] = MaxProgramBytes + 1; }
    SECTION("addition overflow") { input["programs"][0]["offset"] = UINT64_MAX; }
    SECTION("constant range") { input["programs"][0]["constants_offset"] = 511; }
    SECTION("narrowing") { input["programs"][0]["constants_offset"] = 0x100000100ULL; }
    SECTION("unknown stage") { input["programs"][0]["stage"] = "unknown"; }
    SECTION("bank words") { input["programs"][0]["bank1_words"] = {1, 2}; }
    SECTION("bad fingerprint") { input["programs"][0]["fingerprint"] = "xyz"; }
    SECTION("bad version") { input["schema_version"] = 2; }
    REQUIRE_THROWS(ParseInput(input.dump(), {}));
}

TEST_CASE("An empty constant section never dereferences its offset", "[video_core][translation_cache]") {
    auto input = Index();
    input["programs"][0]["constants_size"] = 0;
    input["programs"][0]["constants_offset"] = UINT32_MAX;
    const auto parsed = ParseInput(input.dump(), {});
    REQUIRE(parsed.programs[0].constants_size == 0);
    REQUIRE(parsed.programs[0].constants_offset == UINT32_MAX);
}

TEST_CASE("A graphics row with more than 256 bank-1 bytes is skipped, not fatal", "[video_core][translation_cache]") {
    auto input = Index();
    auto row = input["programs"][0];
    row["container"] = "large.bin";
    row["constants_offset"] = 0;
    row["constants_size"] = MaxBank1Bytes + 4;
    input["programs"].push_back(row);
    const auto parsed = ParseInput(input.dump(), {});
    REQUIRE(parsed.programs.size() == 2);
    REQUIRE(parsed.programs[0].skip_reason.empty());
    REQUIRE(parsed.programs[1].skip_reason == "bank1_too_large");
    // Out-of-range sections are still malformed whatever their size.
    input["programs"][1]["constants_offset"] = 400;
    REQUIRE_THROWS(ParseInput(input.dump(), {}));
}

TEST_CASE("Offline program index prioritizes tiers stably", "[video_core][translation_cache]") {
    auto input = Index();
    auto row = input["programs"][0];
    input["programs"][0]["tier"] = 2;
    row["container"] = "first.bin";
    row["tier"] = 1;
    row["bank1_words"] = {123};
    input["programs"].push_back(row);
    row["container"] = "second.bin";
    input["programs"].push_back(row);
    const auto parsed = ParseInput(input.dump(), {});
    REQUIRE(parsed.programs[0].container == "first.bin");
    REQUIRE(parsed.programs[1].container == "second.bin");
    REQUIRE(parsed.programs[2].container == "code.bin");
    REQUIRE(parsed.programs[0].bank1_words == std::vector<u32>{123});
}

TEST_CASE("Recipe selection counts variants and preserves full emission inputs", "[video_core][translation_cache]") {
    const auto path = std::filesystem::temp_directory_path() / "lichii_offline_recipes.bin";
    std::filesystem::remove(path);
    const Digest config{87, 32};
    auto common = DefaultEmissionRecipe(Shader::Stage::Fragment);
    common.bindings.texture = 7;
    common.bindings.unified = 11;
    common.runtime.previous_stage_stores.mask.reset(128);
    common.runtime.per_primitive_generics.set(3);
    common.runtime.previous_stage_legacy_stores_mapping[Shader::IR::Attribute::ColorFrontDiffuseR] =
        Shader::IR::Attribute::Generic0X;
    common.runtime.fixed_state_point_size = 2.5f;
    common.runtime.alpha_test_func = Shader::CompareFunction::Greater;
    common.runtime.alpha_test_reference = 0.25f;
    common.runtime.xfb_count = 1;
    common.runtime.xfb_varyings[0] = {1, 2, 3, 4, 1};
    auto rare = common;
    rare.bindings.texture = 8;
    {
        Store store;
        store.Open(path, config);
        for (u64 i = 0; i < 3; ++i) {
            ProgramMeta meta{};
            meta.stage = Shader::Stage::Fragment;
            const auto entry = store.Insert({i, 3}, {}, meta);
            const auto& recipe = i == 0 ? rare : common;
            const auto key = MakeVariantKey(entry->id, DigestRuntimeInfo(recipe.runtime), recipe.bindings);
            const Variant variant{.spirv = {0x07230203, 0x10000, 0, 1, 0}};
            store.InsertVariant(*entry, key, variant, &recipe);
            store.InsertVariant(*entry, key, variant, &recipe);
        }
    }
    const auto length = std::filesystem::file_size(path);
    {
        std::ofstream file(path, std::ios::binary | std::ios::app);
        file.write("partial", 7);
    }
    const auto recipes = Store::ReadRecipes(path, config);
    REQUIRE(std::filesystem::file_size(path) == length + 7);
    const auto& chosen = recipes[static_cast<size_t>(Shader::Stage::Fragment)];
    REQUIRE(chosen.bindings.texture == common.bindings.texture);
    REQUIRE(chosen.bindings.unified == common.bindings.unified);
    REQUIRE(DigestRuntimeInfo(chosen.runtime) == DigestRuntimeInfo(common.runtime));
    REQUIRE(DigestRuntimeInfo(recipes[0].runtime) ==
            DigestRuntimeInfo(DefaultEmissionRecipe(Shader::Stage::VertexB).runtime));
    REQUIRE(recipes[3].runtime.input_topology == Shader::InputTopology::Triangles);
    REQUIRE_THROWS(Store::ReadRecipes(path, {1, 2}));
    REQUIRE(std::filesystem::file_size(path) == length + 7);
    std::filesystem::remove(path);
}

TEST_CASE("Equal recipe frequencies choose the lowest stable digest", "[video_core][translation_cache]") {
    const auto path = std::filesystem::temp_directory_path() / "lichii_offline_recipe_tie.bin";
    std::filesystem::remove(path);
    const Digest config{12, 45};
    auto a = DefaultEmissionRecipe(Shader::Stage::VertexB);
    auto b = a;
    b.bindings.texture = 1;
    const auto ak = MakeVariantKey({0, 0}, DigestRuntimeInfo(a.runtime), a.bindings);
    const auto bk = MakeVariantKey({0, 0}, DigestRuntimeInfo(b.runtime), b.bindings);
    const auto& expected = std::tie(ak.lo, ak.hi) < std::tie(bk.lo, bk.hi) ? a : b;
    for (bool reverse : {false, true}) {
        std::filesystem::remove(path);
        {
            Store store;
            store.Open(path, config);
            ProgramMeta meta{};
            meta.stage = Shader::Stage::VertexB;
            const auto entry = store.Insert({1, 2}, {}, meta);
            for (const auto* recipe : {reverse ? &a : &b, reverse ? &b : &a}) {
                store.InsertVariant(*entry,
                    MakeVariantKey(entry->id, DigestRuntimeInfo(recipe->runtime), recipe->bindings),
                    {.spirv = {0x07230203, 0x10000, 0, 1, 0}}, recipe);
            }
        }
        REQUIRE(Store::ReadRecipes(path, config)[0].bindings.texture == expected.bindings.texture);
    }
    std::filesystem::remove(path);
}
