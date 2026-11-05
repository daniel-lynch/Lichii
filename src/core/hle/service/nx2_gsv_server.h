// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstdlib>
#include <memory>

#include "common/scope_exit.h"
#include "core/hle/service/server_manager.h"

namespace Core {
class System;
}

namespace Service {

// The server for the Switch 2 graphics system sessions on a host thread of its own, with
// EDEN_NX2_GSV_THREAD=1 (off by default): nx2:gsv (ISelfController 2000) and every session handed
// out from it (vi2, gpu, syncpt, nvsrv, video decoder, ...). By default they are registered with
// AM's server, a guest service thread on emulated core 3: vi2's present (229) waits there for the
// previous frame's GPU fence and then for the next vblank, and for that time core 3 runs no game
// thread and answers no other graphics request (GPU submissions through nvsrv included). One
// thread, so these handlers stay serialized among themselves as before.
//
// Ported from Orchid e36aac966f, which has it on by default (EDEN_NX2_GSV_HOST_THREAD=0 off).
inline std::atomic<ServerManager*> g_nx2_gsv_server{};

[[nodiscard]] inline bool Nx2GsvHostThreadEnabled() {
    static const bool enabled = [] {
        const char* const value = std::getenv("EDEN_NX2_GSV_THREAD");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

/// Runs the Switch 2 graphics system server on the calling host thread until emulation stops.
inline void RunNx2GsvServer(Core::System& system) {
    auto server_manager = std::make_unique<ServerManager>(system);
    g_nx2_gsv_server.store(server_manager.get(), std::memory_order_release);
    // Cleared once the server stops (or is refused at shutdown): later sessions fall back to the
    // server of the session that creates them.
    SCOPE_EXIT {
        g_nx2_gsv_server.store(nullptr, std::memory_order_release);
    };
    ServerManager::RunServer(std::move(server_manager));
}

} // namespace Service
