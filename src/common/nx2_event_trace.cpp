// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include "common/logging.h"
#include "common/nx2_event_trace.h"

namespace Common::Nx2EventTrace {
namespace {
using SteadyClock = std::chrono::steady_clock;
using SystemClock = std::chrono::system_clock;

struct OpenFile {
    std::string directory;
    std::FILE* file{};
};

std::mutex mutex;
SteadyClock::time_point session_start = SteadyClock::now();
std::vector<OpenFile> files;
std::vector<std::string> failed_directories;

std::mutex names_mutex;
std::unordered_map<u32, std::string> user_names;

std::tm LocalTime(SystemClock::time_point wall) {
    const std::time_t seconds = SystemClock::to_time_t(wall);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    return local;
}

int Milliseconds(SystemClock::time_point wall) {
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(wall.time_since_epoch()).count() %
        1000;
    return static_cast<int>(ms < 0 ? ms + 1000 : ms);
}

u32 Rotl(u32 value, int shift) {
    return (value << shift) | (value >> (32 - shift));
}

/// The step enum values: the shapes their names take (the GameData enums name steps NotReady,
/// Ready, Step01..StepNN with _NN and _preNN sub-steps, and Complete).
const std::unordered_map<u32, std::string>& StepNames() {
    static const auto names = [] {
        std::unordered_map<u32, std::string> map;
        const auto add = [&](std::string name) {
            map.try_emplace(Murmur3(name), std::move(name));
        };
        add("NotReady");
        add("Ready");
        add("Complete");
        for (int step = 0; step <= 40; ++step) {
            add(fmt::format("Step{:02}", step));
            for (int sub = 0; sub <= 20; ++sub) {
                add(fmt::format("Step{:02}_{:02}", step, sub));
                add(fmt::format("Step{:02}_pre{:02}", step, sub));
            }
        }
        return map;
    }();
    return names;
}
} // Anonymous namespace

Target ParseTarget(const char* value) {
    if (value == nullptr || value[0] == '\0' || std::strcmp(value, "0") == 0) {
        return {};
    }
    if (std::strcmp(value, "1") == 0) {
        return {Sink::MainLog, {}};
    }
    return {Sink::Directory, value};
}

const Target& RomfsTarget() {
    static const Target target = ParseTarget(std::getenv("EDEN_NX2_ROMFS_TRACE"));
    return target;
}

const Target& GameDataTarget() {
    static const Target target = ParseTarget(std::getenv("EDEN_NX2_GAMEDATA_TRACE"));
    return target;
}

std::optional<RomfsKind> RomfsKindOfPath(std::string_view path) {
    if (path.starts_with('/')) {
        path.remove_prefix(1);
    }
    // A file under `dir` (any depth below it), not the directory itself.
    const auto under = [&](std::string_view dir) {
        return path.size() > dir.size() && path.starts_with(dir) && path.back() != '/';
    };
    if (under("Event/EventFlow/")) {
        const std::string_view name = path.substr(path.rfind('/') + 1);
        if (name.find(".bfevfl") == std::string_view::npos) {
            return std::nullopt;
        }
        return name.starts_with("Dm_") ? RomfsKind::Timeline : RomfsKind::EventFlow;
    }
    if (under("Event/Timeline/")) {
        return RomfsKind::Timeline;
    }
    if (under("Event/Movie/")) {
        return RomfsKind::Movie;
    }
    if (under("Banc/")) {
        return RomfsKind::Banc;
    }
    if (under("GameData/")) {
        return RomfsKind::GameData;
    }
    return std::nullopt;
}

std::string_view Name(RomfsKind kind) {
    switch (kind) {
    case RomfsKind::EventFlow:
        return "evfl";
    case RomfsKind::Timeline:
        return "timeline";
    case RomfsKind::Movie:
        return "movie";
    case RomfsKind::Banc:
        return "banc";
    case RomfsKind::GameData:
        return "gamedata";
    }
    return "?";
}

u32 Murmur3(std::string_view text, u32 seed) {
    constexpr u32 c1 = 0xcc9e2d51;
    constexpr u32 c2 = 0x1b873593;
    const auto* data = reinterpret_cast<const unsigned char*>(text.data());
    const size_t length = text.size();
    const size_t blocks = length / 4;
    u32 hash = seed;
    for (size_t i = 0; i < blocks; ++i) {
        u32 k = u32{data[i * 4]} | (u32{data[i * 4 + 1]} << 8) | (u32{data[i * 4 + 2]} << 16) |
                (u32{data[i * 4 + 3]} << 24);
        k *= c1;
        k = Rotl(k, 15);
        k *= c2;
        hash ^= k;
        hash = Rotl(hash, 13);
        hash = hash * 5 + 0xe6546b64;
    }
    const unsigned char* tail = data + blocks * 4;
    u32 k = 0;
    switch (length & 3) {
    case 3:
        k ^= u32{tail[2]} << 16;
        [[fallthrough]];
    case 2:
        k ^= u32{tail[1]} << 8;
        [[fallthrough]];
    case 1:
        k ^= u32{tail[0]};
        k *= c1;
        k = Rotl(k, 15);
        k *= c2;
        hash ^= k;
        break;
    default:
        break;
    }
    hash ^= static_cast<u32>(length);
    hash ^= hash >> 16;
    hash *= 0x85ebca6b;
    hash ^= hash >> 13;
    hash *= 0xc2b2ae35;
    hash ^= hash >> 16;
    return hash;
}

std::optional<std::string> NameOfHash(u32 hash) {
    if (const auto& steps = StepNames(); steps.contains(hash)) {
        return steps.at(hash);
    }
    std::scoped_lock lock{names_mutex};
    if (const auto it = user_names.find(hash); it != user_names.end()) {
        return it->second;
    }
    return std::nullopt;
}

std::string DescribeHash(u32 hash) {
    if (auto name = NameOfHash(hash)) {
        return fmt::format("{} ({:#010x})", *name, hash);
    }
    return fmt::format("{:#010x}", hash);
}

size_t LoadNamesFile(const char* path) {
    if (path == nullptr || path[0] == '\0') {
        return 0;
    }
    std::ifstream in{std::filesystem::path{path}};
    if (!in) {
        LOG_WARNING(Loader, "Event trace: cannot read the names file {}", path);
        return 0;
    }
    size_t added = 0;
    std::string line;
    std::scoped_lock lock{names_mutex};
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        if (line.empty() || line.front() == '#') {
            continue;
        }
        added += user_names.try_emplace(Murmur3(line), line).second ? 1 : 0;
    }
    return added;
}

void BeginSession() {
    std::scoped_lock lock{mutex};
    for (auto& opened : files) {
        std::fclose(opened.file);
    }
    files.clear();
    failed_directories.clear();
    session_start = SteadyClock::now();
}

std::string FormatLine(SystemClock::time_point wall, SteadyClock::duration since_session,
                       std::string_view text) {
    const std::tm local = LocalTime(wall);
    return fmt::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03} +{:.3f} {}",
                       local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour,
                       local.tm_min, local.tm_sec, Milliseconds(wall),
                       std::chrono::duration<double>(since_session).count(), text);
}

std::string SessionFileName(SystemClock::time_point wall) {
    const std::tm local = LocalTime(wall);
    return fmt::format("event-trace-{:04}{:02}{:02}-{:02}{:02}{:02}-{:03}.log",
                       local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour,
                       local.tm_min, local.tm_sec, Milliseconds(wall));
}

void Write(const Target& target, std::string_view text) {
    if (!target.On()) {
        return;
    }
    const auto wall = SystemClock::now();
    std::string line;
    {
        std::scoped_lock lock{mutex};
        line = FormatLine(wall, SteadyClock::now() - session_start, text);
        if (target.sink == Sink::Directory) {
            auto it = std::find_if(files.begin(), files.end(), [&](const OpenFile& opened) {
                return opened.directory == target.directory;
            });
            const bool failed = std::find(failed_directories.begin(), failed_directories.end(),
                                          target.directory) != failed_directories.end();
            if (it == files.end() && !failed) {
                std::error_code error;
                const std::filesystem::path directory{target.directory};
                std::filesystem::create_directories(directory, error);
                const auto path = directory / SessionFileName(wall);
                std::FILE* file = std::fopen(path.string().c_str(), "a");
                if (file == nullptr) {
                    failed_directories.push_back(target.directory);
                    LOG_WARNING(Loader, "Event trace: cannot open {}; tracing to the log instead",
                                path.string());
                } else {
                    LOG_INFO(Loader, "Event trace: writing {}", path.string());
                    it = files.insert(files.end(), OpenFile{target.directory, file});
                }
            }
            if (it != files.end()) {
                std::fputs(line.c_str(), it->file);
                std::fputc('\n', it->file);
                // Line by line, so a crash or a kill keeps everything up to the last event.
                std::fflush(it->file);
                return;
            }
        }
    }
    LOG_INFO(Loader, "Event trace: {}", line);
}

} // namespace Common::Nx2EventTrace
