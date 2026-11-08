// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

#include "common/common_types.h"

namespace VideoCommon::PipelineDiagnostics {

inline std::atomic<u64> presents{};
inline u64 PresentCount() {
    return presents.load(std::memory_order_relaxed);
}
inline void ResetPresentCount() {
    presents.store(0, std::memory_order_relaxed);
}
inline void AdvancePresentCount() {
    presents.fetch_add(1, std::memory_order_relaxed);
}

// Sample cumulative elapsed waits, including active waits. Differencing samples never counts
// the already-reported portion again when a wait ends, even across several report windows.
class WaitCounter {
public:
    using Clock = std::chrono::steady_clock;
    struct Sample {
        std::chrono::nanoseconds total{};
        u64 active{};
    };
    u64 Begin(std::optional<Clock::time_point> at = std::nullopt) {
        std::scoped_lock lock{mutex};
        const u64 id = ++serial;
        active.emplace(id, at.value_or(Clock::now()));
        return id;
    }
    void End(u64 id, std::optional<Clock::time_point> at = std::nullopt) {
        std::scoped_lock lock{mutex};
        const auto now = at.value_or(Clock::now());
        if (const auto it = active.find(id); it != active.end()) {
            completed += now - it->second;
            active.erase(it);
        }
    }
    Sample Read(std::optional<Clock::time_point> at = std::nullopt) const {
        std::scoped_lock lock{mutex};
        const auto now = at.value_or(Clock::now());
        auto total = completed;
        for (const auto& [id, begin] : active) {
            total += now > begin ? now - begin : Clock::duration{};
        }
        return {std::chrono::duration_cast<std::chrono::nanoseconds>(total), active.size()};
    }

private:
    mutable std::mutex mutex;
    Clock::duration completed{};
    u64 serial{};
    std::unordered_map<u64, Clock::time_point> active;
};

inline std::filesystem::path MarkFilePath(const char* value) {
    if (!value) {
        return {};
    }
    std::string word{value};
    const auto first = word.find_first_not_of(" \t\r\n\v\f");
    if (first == std::string::npos) {
        return {};
    }
    word = word.substr(first, word.find_last_not_of(" \t\r\n\v\f") - first + 1);
    for (char& c : word) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return word == "0" || word == "off" || word == "false" || word == "none"
               ? std::filesystem::path{}
               : std::filesystem::path{value};
}

// Content comparison also notices same-size rewrites on filesystems with coarse timestamps.
// Read only regular files, at most 4096 bytes, and escape line breaks to keep one log record.
class SceneMarker {
public:
    explicit SceneMarker(std::filesystem::path path_) : path{std::move(path_)} {}

    std::optional<std::string> Poll() {
        std::error_code error;
        if (path.empty() || !std::filesystem::is_regular_file(path, error)) {
            return std::nullopt;
        }
        std::ifstream file{path, std::ios::binary};
        if (!file) {
            return std::nullopt;
        }
        std::string text(4097, '\0');
        file.read(text.data(), static_cast<std::streamsize>(text.size()));
        if (file.bad()) {
            return std::nullopt;
        }
        text.resize(static_cast<size_t>(file.gcount()));
        if (previous && *previous == text) {
            return std::nullopt;
        }
        previous = text;
        const bool truncated = text.size() > 4096;
        text.resize((std::min)(text.size(), size_t{4096}));
        std::string line;
        for (const char byte : text) {
            const auto c = static_cast<unsigned char>(byte);
            if (c == '\n') {
                line += "\\n";
            } else if (c == '\r') {
                line += "\\r";
            } else if (c == '\\') {
                line += "\\\\";
            } else if (c < 0x20 || c == 0x7f) {
                line += '?';
            } else {
                line += static_cast<char>(c);
            }
        }
        if (truncated) {
            line += " [truncated at 4096 bytes]";
        }
        return line;
    }

private:
    std::filesystem::path path;
    std::optional<std::string> previous;
};

} // namespace VideoCommon::PipelineDiagnostics
