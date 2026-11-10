// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <filesystem>

namespace Core::OotCoop::Link {
class DisconnectTrigger {
public:
    enum class Stale { Absent, Removed, Failed };
    explicit DisconnectTrigger(std::filesystem::path eden_dir);
    // Errors hold offline; dangling symlinks also count as requests.
    [[nodiscard]] bool Present() const;
    [[nodiscard]] Stale TakeStale() const;
private:
    std::filesystem::path path;
};
} // namespace Core::OotCoop::Link
