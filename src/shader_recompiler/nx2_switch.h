// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

#include "common/logging.h"

namespace Shader {

/// The value of a boolean EDEN_NX2_* switch: nullopt when unset, true for an empty value, 1, on or
/// true, false for 0, off, false or none (trimmed, any case). Anything else is logged and false, so
/// "=0" never turns a switch on.
[[nodiscard]] inline std::optional<bool> ParseNx2Switch(std::string_view name, const char* value) {
    if (value == nullptr) {
        return std::nullopt;
    }
    std::string_view raw{value};
    constexpr std::string_view whitespace{" \t\r\n\v\f"};
    const size_t first = raw.find_first_not_of(whitespace);
    raw = first == std::string_view::npos
              ? std::string_view{}
              : raw.substr(first, raw.find_last_not_of(whitespace) - first + 1);
    std::string word{raw};
    for (char& letter : word) {
        if (letter >= 'A' && letter <= 'Z') {
            letter = static_cast<char>(letter - 'A' + 'a');
        }
    }
    if (word.empty() || word == "1" || word == "on" || word == "true") {
        return true;
    }
    if (word != "0" && word != "off" && word != "false" && word != "none") {
        LOG_WARNING(Shader, "{}=\"{}\" is not a switch value; treated as 0", name, value);
    }
    return false;
}

/// ParseNx2Switch of the variable's current value, unset meaning fallback
[[nodiscard]] inline bool Nx2Switch(const char* name, bool fallback) {
    return ParseNx2Switch(name, std::getenv(name)).value_or(fallback);
}

} // namespace Shader
