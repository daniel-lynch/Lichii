// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

#include "common/common_types.h"

/// Progression traces for game research (Ocarina of Time first): which event flows, cutscene
/// timelines, scene files and GameData files the game reads (EDEN_NX2_ROMFS_TRACE), and which
/// quest steps and HavePlayedEvent flags it sets (EDEN_NX2_GAMEDATA_TRACE, core/arm/dynarmic/
/// oot_gamedata_trace.h). Both are off unless their switch is set, and only ever log: nothing here
/// changes what the game reads or runs. Thread safe.
namespace Common::Nx2EventTrace {

enum class Sink : u8 {
    Off,
    MainLog,   ///< =1: lines go to the emulator's log
    Directory, ///< =<dir>: lines go to <dir>/event-trace-<local time>.log, one file per session
};

struct Target {
    Sink sink = Sink::Off;
    std::string directory;

    [[nodiscard]] bool On() const {
        return sink != Sink::Off;
    }
    bool operator==(const Target&) const = default;
};

/// A switch value: unset, empty or "0" is off, "1" the main log, anything else a directory.
[[nodiscard]] Target ParseTarget(const char* value);

/// EDEN_NX2_ROMFS_TRACE and EDEN_NX2_GAMEDATA_TRACE, read once per process.
[[nodiscard]] const Target& RomfsTarget();
[[nodiscard]] const Target& GameDataTarget();

enum class RomfsKind : u8 {
    EventFlow, ///< Event/EventFlow/<name>.bfevfl[.zs], <name> not a cutscene
    Timeline,  ///< Event/EventFlow/Dm_*.bfevfl* (a cutscene's flow and TLIN timeline),
               ///< Event/Timeline/*, CameraAnimation/Dm_* (a cutscene's camera)
    Movie,     ///< Event/Movie/* (pre-rendered cutscenes)
    Banc,      ///< Banc/* (scene placement and parameters)
    GameData,  ///< GameData/* (the flag list)
};

/// Which kind of progression file a RomFS path is (a leading '/' is ignored), or nothing.
[[nodiscard]] std::optional<RomfsKind> RomfsKindOfPath(std::string_view path);
[[nodiscard]] std::string_view Name(RomfsKind kind);

/// The game's GameData name hash (MurmurHash3 x86 32-bit, seed 0).
[[nodiscard]] u32 Murmur3(std::string_view text, u32 seed = 0);

/// The quest-step enum value whose hash this is, from the shapes the step names take (NotReady,
/// Ready, Complete, StepNN, StepNN_MM, StepNN_preMM), or a name from EDEN_NX2_GAMEDATA_NAMES.
[[nodiscard]] std::optional<std::string> NameOfHash(u32 hash);
/// "Step07 (0xee867c27)" for a known hash, "0x1234abcd" otherwise.
[[nodiscard]] std::string DescribeHash(u32 hash);

/// EDEN_NX2_GAMEDATA_NAMES=<file>: one name per line (challenge, flag or event names, from the
/// user's own extract), so their hashes print by name. Read once; returns how many were added.
size_t LoadNamesFile(const char* path);

/// Starts an emulation session (Core::System::Load): the session clock restarts and the next line
/// written to a directory opens a new file.
void BeginSession();

/// "2026-11-06 23:10:04.123 +612.345 <text>": local wall time and seconds since the session began.
[[nodiscard]] std::string FormatLine(std::chrono::system_clock::time_point wall,
                                     std::chrono::steady_clock::duration since_session,
                                     std::string_view text);
/// "event-trace-20261106-231004-123.log".
[[nodiscard]] std::string SessionFileName(std::chrono::system_clock::time_point wall);

/// Writes one line to `target` (nothing when it is off). A directory's file is opened on the
/// session's first line and line buffered: one fopen per session and one fputs per line. A
/// directory that cannot be opened falls back to the main log.
void Write(const Target& target, std::string_view text);

} // namespace Common::Nx2EventTrace
