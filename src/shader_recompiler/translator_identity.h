// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <string_view>

#include "common/common_types.h"

namespace Shader {

/// SHA-256, in lowercase hexadecimal, of the translator's source and of the build facts that
/// can change its output (CMakeModules/TranslatorIdentity.cmake), computed at build time.
[[nodiscard]] std::string_view TranslatorSourceHash();

/// Number of source files in TranslatorSourceHash.
[[nodiscard]] u32 TranslatorSourceFileCount();

/// Predefined macros of the translator's own compilation that can change the code generated
/// from the same source (compiler, standard library, optimisation, floating-point model, ISA).
[[nodiscard]] std::string_view TranslatorCompileFacts();

/// The translator's identity for caches of its output: equal in every executable built from
/// the same source with the same facts, and never derived from the running process.
[[nodiscard]] std::string TranslatorIdentity();

} // namespace Shader
