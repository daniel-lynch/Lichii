// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <fstream>
#include <nlohmann/json.hpp>
#include <catch2/catch_test_macros.hpp>
#include "video_core/offline_shader_producer.h"
#include "core/launch_params.h"

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
    common.runtime.xfb_count = 33;
    common.runtime.xfb_varyings[32] = {1, 2, 8, 4, 1};
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

TEST_CASE("Offline production signals request cancellation without terminating",
          "[video_core][translation_cache]") {
    {
        SignalGuard signals;
        REQUIRE(StopSignal() == 0);
        std::raise(SIGTERM);
        REQUIRE(StopSignal() == SIGTERM);
    }
    {
        SignalGuard signals;
        REQUIRE(StopSignal() == 0);
        std::raise(SIGINT);
        REQUIRE(StopSignal() == SIGINT);
    }
    // Leave the producer ready for other tests in this process.
    SignalGuard reset;
}

TEST_CASE("Recipe catalogs retain exact variants and rank runtime recipes by class",
          "[video_core][translation_cache]") {
    const auto path = std::filesystem::temp_directory_path() / "lichii_offline_recipe_catalog.bin";
    std::filesystem::remove(path);
    const Digest source_config{27, 91};
    ProgramMeta meta{};
    meta.stage = Shader::Stage::VertexB;
    auto common = DefaultEmissionRecipe(meta.stage);
    common.bindings.texture = 2;
    auto rare = common;
    rare.bindings.texture = 3;
    auto fallback = common;
    fallback.bindings.texture = 99;
    auto other_class = common;
    other_class.bindings.texture = 4;
    {
        Store store;
        store.Open(path, source_config);
        for (u64 i = 0; i < 6; ++i) {
            auto info = meta;
            if (i == 5) info.info.loads.mask.set(128);
            const auto entry = store.Insert({i, 1}, {}, info);
            const auto& recipe = i < 3 ? common : i == 3 ? rare : i == 4 ? fallback : other_class;
            const Variant variant{.spirv = {0x07230203, 0x10000, 0, 1, 0},
                                  .fallback_recipe = i == 4};
            const auto key = MakeVariantKey(entry->id, DigestRuntimeInfo(recipe.runtime), recipe.bindings);
            REQUIRE(store.InsertVariant(*entry, key, variant, &recipe));
            REQUIRE(store.InsertVariant(*entry, key, variant, &recipe));
        }
    }
    const auto size = std::filesystem::file_size(path);
    {
        std::ofstream file(path, std::ios::binary | std::ios::app);
        file << "partial";
    }
    const auto catalog = Store::ReadRecipeCatalog(path, {100, 200});
    REQUIRE(catalog.source_config == source_config);
    REQUIRE(std::filesystem::file_size(path) == size + 7);
    const auto exact = catalog.Select({3, 1}, meta.stage, meta.info, 1, RecipeBaseline::Always);
    REQUIRE(exact.size() == 2);
    REQUIRE(exact[0].bindings.texture == 3);
    REQUIRE(exact[1].bindings.texture == 0);
    const auto ranked = catalog.Select({99, 1}, meta.stage, meta.info, 3);
    REQUIRE(ranked.size() == 3);
    REQUIRE(ranked[0].bindings.texture == 2);
    REQUIRE(ranked[1].bindings.texture == 3);
    meta.info.loads.mask.set(128);
    const auto classified = catalog.Select({99, 1}, meta.stage, meta.info, 3);
    REQUIRE(classified.size() == 2);
    REQUIRE(classified[0].bindings.texture == 4);
    REQUIRE(classified[1].bindings.texture == 0);
    std::filesystem::remove(path);
}

TEST_CASE("Recipe policy defaults to two observations and attributes canonical duplicates",
          "[video_core][translation_cache]") {
    REQUIRE(Core::LaunchParams{}.shader_recipe_count == 2);
    REQUIRE(Core::LaunchParams{}.shader_recipe_baseline == "auto");
    REQUIRE(DefaultRecipeCount == 2);
    const auto stage = Shader::Stage::VertexB;
    const Digest key{17, 18};
    RecipeCatalog catalog;
    auto recipe = DefaultEmissionRecipe(stage);
    auto add = [&](auto& list, u32 binding) {
        recipe.bindings.texture = binding;
        list.push_back({recipe, {}, 1});
    };
    auto& exact = catalog.exact[key];
    add(exact, 1);
    add(exact, 2);
    add(exact, 3);
    auto& classes = catalog.classes[RecipeClass(stage, nullptr)];
    add(classes, 4);
    add(classes, 5);
    add(classes, 6);
    const auto defaults = catalog.SelectWithSources(key, stage, {});
    REQUIRE(defaults.size() == 2);
    REQUIRE(defaults[0].source == RecipeSource::ExactStage);
    REQUIRE(defaults[1].recipe.bindings.texture == 2);
    REQUIRE(catalog.Select(key, stage, {}, 2, RecipeBaseline::Always).size() == 3);
    REQUIRE(catalog.Select(key, stage, {}, 2, RecipeBaseline::Never).size() == 2);
    const auto fallback = catalog.SelectWithSources({99, 99}, stage, {});
    REQUIRE(fallback.size() == 3);
    REQUIRE(fallback[0].source == RecipeSource::Class);
    REQUIRE(fallback[2].source == RecipeSource::Baseline);
    REQUIRE(catalog.Select({99, 99}, stage, {}, 2, RecipeBaseline::Never).size() == 2);
    catalog.classes.clear();
    exact.resize(2);
    exact[1] = exact[0];
    // This unused input differs before canonicalization, but produces the same variant key.
    exact[1].recipe.runtime.generic_input_types[0] = Shader::AttributeType::SignedInt;
    const auto deduped = catalog.SelectWithSources(key, stage, {});
    REQUIRE(deduped.size() == 1);
    REQUIRE(deduped[0].source == RecipeSource::ExactStage);
    exact[0].recipe = DefaultEmissionRecipe(stage);
    exact.resize(1);
    const auto baseline_duplicate = catalog.SelectWithSources(key, stage, {}, 2, RecipeBaseline::Always);
    REQUIRE(baseline_duplicate.size() == 1);
    REQUIRE(baseline_duplicate[0].source == RecipeSource::ExactStage);
    catalog.exact.clear();
    REQUIRE(catalog.Select(key, stage, {}).size() == 1);
    REQUIRE(catalog.Select(key, stage, {}, 2, RecipeBaseline::Never).empty());
}

TEST_CASE("Shader report includes skips and all import limits", "[video_core][translation_cache]") {
    Summary summary;
    summary.seen = 7;
    summary.cancelled = true;
    summary.skipped["unsupported_stage"] = 4;
    summary.import_truncated_entries = 8;
    summary.import_skipped_records = 9;
    summary.import_scanned_records = 100;
    summary.import_eligible_records = 20;
    summary.import_admitted_records = 11;
    summary.import_framing_skipped_bytes = 99999;
    const auto path = std::filesystem::temp_directory_path() / "lichii_report_test.json";
    WriteReport(path, summary);
    std::ifstream file(path);
    const auto report = nlohmann::json::parse(file);
    REQUIRE(report["seen"] == 7);
    REQUIRE(report["cancelled"] == true);
    REQUIRE(report["skips"]["unsupported_stage"] == 4);
    REQUIRE(report["import"]["truncated_entries"] == 8);
    REQUIRE(report["import"]["skipped_records"] == 9);
    REQUIRE(report["import"]["scanned_records"] == 100);
    REQUIRE(report["import"]["eligible_records"] == 20);
    REQUIRE(report["import"]["admitted_records"] == 11);
    REQUIRE(report["import"]["framing_skipped_bytes"] == 99999);
    REQUIRE(report["sources"]["class"]["modules"] == 0);
    file.close();
    std::filesystem::remove(path);
    REQUIRE_THROWS(WriteReport(path / "missing-parent.json", summary));
}
