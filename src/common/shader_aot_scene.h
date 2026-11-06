// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "common/common_types.h"

/// Which scene the game is loading, for shader AOT translation (use_shader_aot_translation with
/// EDEN_NX2_AOT_MANIFEST). The RomFS builder wraps the scene placement files
/// (Banc/<Dir>/<Scene>_Static|_Dynamic.bcett.byml.zs) and posts the scene name when the game
/// reads one; the shader pool index then loads that scene's manifest. Thread safe.
namespace Common::ShaderAotScene {

/// Whether scene hints are wanted: the AOT switch is on and a manifest set is named.
[[nodiscard]] bool Wanted();

/// The RomFS builder wrapped this many scene placement files (0: the scene cannot be known).
void SetHookedFiles(size_t count);
[[nodiscard]] size_t HookedFiles();

/// The game read a placement file of `scene`. A repeat of the current scene changes nothing.
void Post(std::string_view scene);

/// Bumped by every Post that names a scene other than the current one (cheap, for every draw).
[[nodiscard]] u64 Generation();

/// The current scene and its generation (0 and empty before the first Post).
[[nodiscard]] std::string Current(u64& generation);

/// The scene name of a RomFS path, or empty when the path is not a scene placement file.
[[nodiscard]] std::string SceneOfPath(std::string_view path);

} // namespace Common::ShaderAotScene
