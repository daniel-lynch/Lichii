// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/logging.h"
#include "video_core/spirv_inline_control.h"

namespace VideoCommon {
namespace {
constexpr u32 SpirvMagic = 0x07230203;
constexpr u32 OpEntryPoint = 15;
constexpr u32 OpFunction = 54;
constexpr u32 OpFunctionEnd = 56;
constexpr u32 OpFunctionCall = 57;
constexpr u32 FunctionControlInline = 0x1;
constexpr u32 FunctionControlDontInline = 0x2;
constexpr std::size_t HeaderWords = 5;

// SPIR-V ExecutionModel values.
SpirvStage StageOfModel(u32 model) {
    switch (model) {
    case 0:
        return SpirvStage::Vertex;
    case 1:
        return SpirvStage::TessControl;
    case 2:
        return SpirvStage::TessEval;
    case 3:
        return SpirvStage::Geometry;
    case 4:
        return SpirvStage::Fragment;
    case 5:
        return SpirvStage::Compute;
    case 5267: // TaskNV
    case 5364: // TaskEXT
        return SpirvStage::Task;
    case 5268: // MeshNV
    case 5365: // MeshEXT
        return SpirvStage::Mesh;
    default:
        return SpirvStage::Unknown;
    }
}
} // Anonymous namespace

std::size_t MarkSharedFunctionsDontInline(std::span<u32> words, std::size_t min_instructions,
                                          std::size_t min_calls) {
    if (words.size() < HeaderWords || words[0] != SpirvMagic) {
        return 0;
    }
    struct Function {
        std::size_t control_word{};
        std::size_t instructions{};
        std::size_t calls{};
    };
    std::unordered_set<u32> entry_points;
    std::unordered_map<u32, Function> functions;
    Function* current = nullptr;
    for (std::size_t index = HeaderWords; index < words.size();) {
        const u32 count = words[index] >> 16;
        const u32 opcode = words[index] & 0xffff;
        if (count == 0 || count > words.size() - index) {
            return 0;
        }
        switch (opcode) {
        case OpEntryPoint:
            if (count < 3) {
                return 0;
            }
            entry_points.insert(words[index + 2]);
            break;
        case OpFunction:
            // OpFunction <result type> <result id> <function control> <function type>
            if (count != 5 || current != nullptr) {
                return 0;
            }
            current = &functions[words[index + 2]];
            current->control_word = index + 3;
            break;
        case OpFunctionCall:
            if (count < 4) {
                return 0;
            }
            ++functions[words[index + 3]].calls;
            break;
        default:
            break;
        }
        if (current != nullptr) {
            ++current->instructions;
        }
        if (opcode == OpFunctionEnd) {
            current = nullptr;
        }
        index += count;
    }
    std::size_t marked = 0;
    for (const auto& [id, function] : functions) {
        if (function.control_word == 0 || entry_points.contains(id) ||
            function.instructions < min_instructions || function.calls < min_calls) {
            continue;
        }
        u32& control = words[function.control_word];
        control = (control & ~FunctionControlInline) | FunctionControlDontInline;
        ++marked;
    }
    return marked;
}

SpirvStage ModuleStage(std::span<const u32> words) {
    if (words.size() < HeaderWords || words[0] != SpirvMagic) {
        return SpirvStage::Unknown;
    }
    bool found = false;
    SpirvStage stage = SpirvStage::Unknown;
    for (std::size_t index = HeaderWords; index < words.size();) {
        const u32 count = words[index] >> 16;
        const u32 opcode = words[index] & 0xffff;
        if (count == 0 || count > words.size() - index) {
            return SpirvStage::Unknown;
        }
        if (opcode == OpFunction) {
            // Entry points precede every function.
            break;
        }
        if (opcode == OpEntryPoint) {
            if (count < 3) {
                return SpirvStage::Unknown;
            }
            const SpirvStage model = StageOfModel(words[index + 1]);
            if (found && model != stage) {
                return SpirvStage::Unknown;
            }
            found = true;
            stage = model;
        }
        index += count;
    }
    return stage;
}

u32 ParseGraphicsNoInlineStages(const char* value) {
    if (value == nullptr) {
        return DefaultGraphicsNoInlineStages;
    }
    // Trimmed of surrounding whitespace and case-folded: "0 ", "OFF" and "Vtm" read as meant.
    std::string_view raw{value};
    constexpr std::string_view whitespace{" \t\r\n\v\f"};
    const std::size_t first = raw.find_first_not_of(whitespace);
    raw = first == std::string_view::npos
              ? std::string_view{}
              : raw.substr(first, raw.find_last_not_of(whitespace) - first + 1);
    std::string text{raw};
    for (char& letter : text) {
        if (letter >= 'A' && letter <= 'Z') {
            letter = static_cast<char>(letter - 'A' + 'a');
        }
    }
    if (text.empty() || text == "1" || text == "on" || text == "true") {
        return DefaultGraphicsNoInlineStages;
    }
    if (text == "0" || text == "none" || text == "off" || text == "false") {
        return 0;
    }
    if (text == "all") {
        return AllGraphicsNoInlineStages;
    }
    u32 mask = 0;
    for (const char letter : text) {
        switch (letter) {
        case 'v':
            mask |= StageBit(SpirvStage::Vertex);
            break;
        case 'c':
            mask |= StageBit(SpirvStage::TessControl);
            break;
        case 'e':
            mask |= StageBit(SpirvStage::TessEval);
            break;
        case 'g':
            mask |= StageBit(SpirvStage::Geometry);
            break;
        case 't':
            mask |= StageBit(SpirvStage::Task);
            break;
        case 'm':
            mask |= StageBit(SpirvStage::Mesh);
            break;
        case 'f':
            mask |= StageBit(SpirvStage::Fragment);
            break;
        default:
            // A value that reads as neither a switch nor a stage list is a mistake: off, said
            // once, rather than a silent default an A/B run would not notice.
            LOG_WARNING(Render_Vulkan,
                        "EDEN_NX2_GRAPHICS_NOINLINE=\"{}\" is not recognised (0/none/off/false, "
                        "1/on/true, all, or stage letters vcegtmf): graphics modules are built as "
                        "emitted",
                        value);
            return 0;
        }
    }
    return mask;
}

u32 GraphicsNoInlineStages() {
    static const u32 mask = ParseGraphicsNoInlineStages(std::getenv("EDEN_NX2_GRAPHICS_NOINLINE"));
    return mask;
}

std::span<const u32> GraphicsModuleCode(std::span<const u32> code, u32 stage_mask,
                                        std::vector<u32>& storage, std::size_t& marked) {
    marked = 0;
    if ((stage_mask & AllGraphicsNoInlineStages) == 0) {
        return code;
    }
    const SpirvStage stage = ModuleStage(code);
    if (stage == SpirvStage::Unknown || stage == SpirvStage::Compute ||
        (stage_mask & AllGraphicsNoInlineStages & StageBit(stage)) == 0) {
        return code;
    }
    storage.assign(code.begin(), code.end());
    marked = MarkSharedFunctionsDontInline(storage);
    if (marked == 0) {
        return code;
    }
    return storage;
}

} // namespace VideoCommon
