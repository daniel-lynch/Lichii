// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "common/common_types.h"

/// Which scene the game is in, for shader AOT translation (use_shader_aot_translation with
/// EDEN_NX2_AOT_MANIFEST). The RomFS builder wraps the scene files and posts the scene name when
/// the game reads one; the shader pool index then loads that scene's manifest. Thread safe.
///
/// Two kinds of file (Ocarina of Time, run 20261106-063753): the game reads a scene's placement
/// files (Banc/<Dir>/<Scene>_Static|_Dynamic.bcett.byml.zs) when it loads the scene and also when
/// it prefetches a neighbour (Hyrule Field's, 7 s after entering Kokiri Forest), and reads
/// Banc/GameBancParam/<Scene>.game__banc__GameBancParam.bgyml when it enters the scene. Before the
/// first entry (boot, title) a placement read names the scene; after it only an entry does.
namespace Common::ShaderAotScene {

/// Starts an emulation session (Core::System::Load, before the game's RomFS is built): forgets
/// the previous session's scene, entry and hooked files. Returns the session's token; wrappers
/// built in an earlier session keep their token and are ignored from now on.
u64 BeginSession();
/// The current session's token.
[[nodiscard]] u64 Session();

/// Whether scene hints are wanted: the AOT switch is on and a manifest set is named.
[[nodiscard]] bool Wanted();

/// The RomFS builder wrapped this many scene files in session `session` (0 after BeginSession:
/// the scene cannot be known).
void SetHookedFiles(size_t count, u64 session);
[[nodiscard]] size_t HookedFiles();

/// The game read a file of `scene`: its parameters (`entered`) or a placement file. A repeat of
/// the current scene changes nothing; a placement read after the first entry is a prefetch.
/// A wrapper of another session posts nothing.
void Post(std::string_view scene, bool entered, u64 session);

/// Placement reads ignored as prefetches so far.
[[nodiscard]] u64 Prefetches();

/// Bumped by every Post that names a scene other than the current one (cheap, for every draw).
[[nodiscard]] u64 Generation();

/// The current scene and its generation (0 and empty before the first Post).
[[nodiscard]] std::string Current(u64& generation);

struct SceneFile {
    std::string scene;
    bool entered; ///< The scene's parameters (read on entry), else a placement file
};
/// The scene of a RomFS path, or nothing when the path is not one of a scene's files.
[[nodiscard]] std::optional<SceneFile> SceneOfPath(std::string_view path);

} // namespace Common::ShaderAotScene
