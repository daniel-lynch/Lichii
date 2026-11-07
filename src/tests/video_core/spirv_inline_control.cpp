// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/spirv_inline_control.h"

namespace {
constexpr u32 Op(u32 opcode, u32 words) {
    return (words << 16) | opcode;
}

// A module skeleton: entry point %1 (main) calls %2 `calls_big` times and %3 `calls_small`
// times; %2 has `big_body` OpNop-like instructions, %3 has one. Returns the words and the
// indices of each function's control word.
struct Module {
    std::vector<u32> words;
    std::size_t main_control{};
    std::size_t big_control{};
    std::size_t small_control{};
};

Module Build(u32 calls_big, u32 calls_small, u32 big_body, u32 big_control = 0, u32 model = 5) {
    Module m;
    auto& w = m.words;
    w = {0x07230203, 0x00010600, 0, 100, 0};
    w.insert(w.end(), {Op(15, 4), model, 1, 0x6e69616d}); // OpEntryPoint <model> %1 "main"
    const auto function = [&](u32 id, u32 control) {
        w.insert(w.end(), {Op(54, 5), 10, id, control, 11});
        return w.size() - 2;
    };
    const auto body = [&](u32 count) {
        w.insert(w.end(), {Op(248, 2), 50}); // OpLabel
        for (u32 i = 0; i < count; ++i) {
            w.insert(w.end(), {Op(1, 3), 10, 60 + i}); // OpUndef
        }
    };
    m.main_control = function(1, 0);
    body(0);
    for (u32 i = 0; i < calls_big; ++i) {
        w.insert(w.end(), {Op(57, 4), 10, 80 + i, 2});
    }
    for (u32 i = 0; i < calls_small; ++i) {
        w.insert(w.end(), {Op(57, 4), 10, 90 + i, 3});
    }
    w.insert(w.end(), {Op(253, 1), Op(56, 1)}); // OpReturn, OpFunctionEnd
    m.big_control = function(2, big_control);
    body(big_body);
    w.insert(w.end(), {Op(253, 1), Op(56, 1)});
    m.small_control = function(3, 0);
    body(1);
    w.insert(w.end(), {Op(253, 1), Op(56, 1)});
    return m;
}
} // Anonymous namespace

TEST_CASE("Large functions called from several places are marked DontInline",
          "[spirv_inline_control]") {
    Module m = Build(3, 5, 200);
    REQUIRE(VideoCommon::MarkSharedFunctionsDontInline(m.words) == 1);
    REQUIRE(m.words[m.big_control] == 0x2);
    REQUIRE(m.words[m.small_control] == 0); // too small: inlining it costs nothing
    REQUIRE(m.words[m.main_control] == 0);  // the entry point is never marked
}

TEST_CASE("A function called once, or a forced Inline one, is handled", "[spirv_inline_control]") {
    Module once = Build(1, 0, 200);
    REQUIRE(VideoCommon::MarkSharedFunctionsDontInline(once.words) == 0);
    REQUIRE(once.words[once.big_control] == 0);

    Module forced = Build(2, 0, 200, 0x1); // Inline
    REQUIRE(VideoCommon::MarkSharedFunctionsDontInline(forced.words) == 1);
    REQUIRE(forced.words[forced.big_control] == 0x2); // Inline cleared, DontInline set
}

TEST_CASE("Thresholds are respected", "[spirv_inline_control]") {
    Module m = Build(2, 0, 50);
    REQUIRE(VideoCommon::MarkSharedFunctionsDontInline(m.words, 128, 2) == 0);
    REQUIRE(VideoCommon::MarkSharedFunctionsDontInline(m.words, 32, 2) == 1);
    Module n = Build(2, 0, 200);
    REQUIRE(VideoCommon::MarkSharedFunctionsDontInline(n.words, 128, 3) == 0);
}

TEST_CASE("Malformed modules are left alone", "[spirv_inline_control]") {
    std::vector<u32> empty;
    REQUIRE(VideoCommon::MarkSharedFunctionsDontInline(empty) == 0);

    Module bad_magic = Build(3, 0, 200);
    bad_magic.words[0] = 0;
    REQUIRE(VideoCommon::MarkSharedFunctionsDontInline(bad_magic.words) == 0);
    REQUIRE(bad_magic.words[bad_magic.big_control] == 0);

    Module truncated = Build(3, 0, 200);
    truncated.words.pop_back();
    truncated.words.back() = Op(1, 9); // the last instruction claims words past the end
    REQUIRE(VideoCommon::MarkSharedFunctionsDontInline(truncated.words) == 0);
    REQUIRE(truncated.words[truncated.big_control] == 0);

    Module zero_count = Build(3, 0, 200);
    zero_count.words[5] = 0; // an instruction with a word count of 0
    REQUIRE(VideoCommon::MarkSharedFunctionsDontInline(zero_count.words) == 0);
}

namespace {
using VideoCommon::SpirvStage;
using VideoCommon::StageBit;
// SPIR-V ExecutionModel values.
constexpr u32 ModelVertex = 0;
constexpr u32 ModelTessControl = 1;
constexpr u32 ModelTessEval = 2;
constexpr u32 ModelGeometry = 3;
constexpr u32 ModelFragment = 4;
constexpr u32 ModelCompute = 5;
constexpr u32 ModelTaskEXT = 5364;
constexpr u32 ModelMeshEXT = 5365;
constexpr u32 ModelMeshNV = 5268;
} // Anonymous namespace

TEST_CASE("A module's stage comes from its entry points", "[spirv_inline_control]") {
    REQUIRE(VideoCommon::ModuleStage(Build(3, 0, 200, 0, ModelVertex).words) == SpirvStage::Vertex);
    REQUIRE(VideoCommon::ModuleStage(Build(3, 0, 200, 0, ModelTessControl).words) ==
            SpirvStage::TessControl);
    REQUIRE(VideoCommon::ModuleStage(Build(3, 0, 200, 0, ModelTessEval).words) ==
            SpirvStage::TessEval);
    REQUIRE(VideoCommon::ModuleStage(Build(3, 0, 200, 0, ModelGeometry).words) ==
            SpirvStage::Geometry);
    REQUIRE(VideoCommon::ModuleStage(Build(3, 0, 200, 0, ModelFragment).words) ==
            SpirvStage::Fragment);
    REQUIRE(VideoCommon::ModuleStage(Build(3, 0, 200, 0, ModelCompute).words) ==
            SpirvStage::Compute);
    REQUIRE(VideoCommon::ModuleStage(Build(3, 0, 200, 0, ModelTaskEXT).words) == SpirvStage::Task);
    REQUIRE(VideoCommon::ModuleStage(Build(3, 0, 200, 0, ModelMeshEXT).words) == SpirvStage::Mesh);
    REQUIRE(VideoCommon::ModuleStage(Build(3, 0, 200, 0, ModelMeshNV).words) == SpirvStage::Mesh);
    REQUIRE(VideoCommon::ModuleStage(Build(3, 0, 200, 0, 99).words) == SpirvStage::Unknown);

    std::vector<u32> empty;
    REQUIRE(VideoCommon::ModuleStage(empty) == SpirvStage::Unknown);
    Module two = Build(3, 0, 200, 0, ModelVertex);
    // A second entry point of another stage, inserted after the first.
    two.words.insert(two.words.begin() + 9, {Op(15, 4), ModelFragment, 2, 0x6e69616d});
    REQUIRE(VideoCommon::ModuleStage(two.words) == SpirvStage::Unknown);
}

TEST_CASE("EDEN_NX2_GRAPHICS_NOINLINE values", "[spirv_inline_control]") {
    using VideoCommon::AllGraphicsNoInlineStages;
    using VideoCommon::DefaultGraphicsNoInlineStages;
    using VideoCommon::ParseGraphicsNoInlineStages;
    REQUIRE(ParseGraphicsNoInlineStages(nullptr) == DefaultGraphicsNoInlineStages);
    REQUIRE(ParseGraphicsNoInlineStages("") == DefaultGraphicsNoInlineStages);
    REQUIRE(ParseGraphicsNoInlineStages("1") == DefaultGraphicsNoInlineStages);
    REQUIRE(ParseGraphicsNoInlineStages("0") == 0);
    REQUIRE(ParseGraphicsNoInlineStages("none") == 0);
    REQUIRE(ParseGraphicsNoInlineStages("all") == AllGraphicsNoInlineStages);
    REQUIRE(ParseGraphicsNoInlineStages("vtm") == (StageBit(SpirvStage::Vertex) |
                                                   StageBit(SpirvStage::Task) |
                                                   StageBit(SpirvStage::Mesh)));
    REQUIRE(ParseGraphicsNoInlineStages("f") == StageBit(SpirvStage::Fragment));
    REQUIRE(ParseGraphicsNoInlineStages("vcegtmf") == AllGraphicsNoInlineStages);
    // Conventional switch words, whitespace and case.
    REQUIRE(ParseGraphicsNoInlineStages("on") == DefaultGraphicsNoInlineStages);
    REQUIRE(ParseGraphicsNoInlineStages("TRUE") == DefaultGraphicsNoInlineStages);
    REQUIRE(ParseGraphicsNoInlineStages("  ") == DefaultGraphicsNoInlineStages);
    REQUIRE(ParseGraphicsNoInlineStages(" 1\n") == DefaultGraphicsNoInlineStages);
    REQUIRE(ParseGraphicsNoInlineStages("off") == 0);
    REQUIRE(ParseGraphicsNoInlineStages("false") == 0);
    REQUIRE(ParseGraphicsNoInlineStages("False") == 0);
    REQUIRE(ParseGraphicsNoInlineStages("NONE") == 0);
    REQUIRE(ParseGraphicsNoInlineStages("0 ") == 0);
    REQUIRE(ParseGraphicsNoInlineStages("\t0") == 0);
    REQUIRE(ParseGraphicsNoInlineStages("ALL") == AllGraphicsNoInlineStages);
    REQUIRE(ParseGraphicsNoInlineStages(" VTM ") == (StageBit(SpirvStage::Vertex) |
                                                     StageBit(SpirvStage::Task) |
                                                     StageBit(SpirvStage::Mesh)));
    // Anything else is rejected: off, never the silent default.
    REQUIRE(ParseGraphicsNoInlineStages("vx") == 0);
    REQUIRE(ParseGraphicsNoInlineStages("v,f") == 0);
    REQUIRE(ParseGraphicsNoInlineStages("v f") == 0);
    REQUIRE(ParseGraphicsNoInlineStages("yes") == 0);
    REQUIRE(ParseGraphicsNoInlineStages("2") == 0);
    // The default leaves fragment shaders, and never names compute.
    REQUIRE((DefaultGraphicsNoInlineStages & StageBit(SpirvStage::Fragment)) == 0);
    REQUIRE((AllGraphicsNoInlineStages & StageBit(SpirvStage::Compute)) == 0);
}

TEST_CASE("Graphics modules are rebuilt only for the selected stages", "[spirv_inline_control]") {
    const auto run = [](u32 model, u32 mask, bool expect_marked) {
        const Module m = Build(3, 0, 200, 0, model);
        std::vector<u32> storage;
        std::size_t marked = 99;
        const std::span<const u32> out =
            VideoCommon::GraphicsModuleCode(m.words, mask, storage, marked);
        if (expect_marked) {
            REQUIRE(marked == 1);
            REQUIRE(out.data() == storage.data());
            REQUIRE(out[m.big_control] == 0x2);
            REQUIRE(m.words[m.big_control] == 0); // the input is never written
        } else {
            REQUIRE(marked == 0);
            REQUIRE(out.data() == m.words.data());
            REQUIRE(out.size() == m.words.size());
        }
    };
    const u32 def = VideoCommon::DefaultGraphicsNoInlineStages;
    run(ModelVertex, def, true);
    run(ModelTessControl, def, true);
    run(ModelTessEval, def, true);
    run(ModelGeometry, def, true);
    run(ModelTaskEXT, def, true);
    run(ModelMeshEXT, def, true);
    run(ModelFragment, def, false);
    run(ModelFragment, VideoCommon::AllGraphicsNoInlineStages, true);
    run(ModelVertex, 0, false);
    run(ModelVertex, StageBit(SpirvStage::Fragment), false);
    // Compute has its own switch and is applied before BuildShader: never here, even if asked.
    run(ModelCompute, def, false);
    run(ModelCompute, ~0u, false);
    run(99, ~0u, false);

    // Nothing to mark: the input is returned.
    const Module small = Build(1, 0, 200, 0, ModelVertex);
    std::vector<u32> storage;
    std::size_t marked = 99;
    const auto out = VideoCommon::GraphicsModuleCode(small.words, def, storage, marked);
    REQUIRE(marked == 0);
    REQUIRE(out.data() == small.words.data());
}
