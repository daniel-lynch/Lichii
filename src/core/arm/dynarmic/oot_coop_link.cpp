// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/arm/dynarmic/oot_coop_link.h"
#include <utility>

namespace Core::OotCoop::Link {
DisconnectTrigger::DisconnectTrigger(std::filesystem::path eden_dir)
    : path{std::move(eden_dir) / "coop-disconnect"} {}
bool DisconnectTrigger::Present() const {
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec == std::errc::no_such_file_or_directory) return false;
    return ec || std::filesystem::exists(status);
}
DisconnectTrigger::Stale DisconnectTrigger::TakeStale() const {
    if (!Present()) return Stale::Absent;
    std::error_code ec;
    const bool removed = std::filesystem::remove(path, ec);
    if (ec || Present()) return Stale::Failed;
    return removed ? Stale::Removed : Stale::Absent;
}
} // namespace Core::OotCoop::Link
