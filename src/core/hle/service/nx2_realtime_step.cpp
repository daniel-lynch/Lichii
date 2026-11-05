// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <bit>
#include <cstdlib>
#include <cstring>
#include <span>

#include "common/hex_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/service/nx2_realtime_step.h"
#include "core/memory.h"

namespace Service::Nx2RealtimeStep {
namespace {

// The build the offsets were read from (the same build the built-in Max Resolution patch pins).
constexpr std::string_view MainBuildId = "E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348";

// Constructor constants the object must still hold before anything is written: a cheap check
// that the cell points at the time object and not at something else.
constexpr u64 OffsetOne = 0x14;   // float 1.0
constexpr u64 OffsetRate = 0x1c;  // int 60
constexpr u64 ObjectSize = 0x20;  // what is read/written: +0x00..+0x1c

} // Anonymous namespace

bool Enabled() {
    static const int env = [] {
        const char* value = std::getenv("EDEN_NX2_REALTIME_STEP");
        if (value == nullptr || value[0] == '\0') {
            return -1;
        }
        return value[0] == '0' ? 0 : 1;
    }();
    if (env != -1) {
        return env == 1;
    }
    return Settings::values.oot_realtime_step.GetValue();
}

bool OotRealtimeStep::Matches(Core::System& system, Kernel::KProcess* process) {
    if (matches) {
        return *matches;
    }
    const u64 program_id = system.GetApplicationProcessProgramID();
    const auto& build_id = system.GetApplicationProcessBuildID();
    const std::string build = Common::HexToString(std::span{build_id}.first(20), true);
    main_base = system.GetApplicationMainModuleBase();
    matches = program_id == ProgramId && build == MainBuildId && main_base != 0 &&
              process != nullptr;
    if (program_id == ProgramId && !*matches) {
        LOG_WARNING(Service,
                    "OoT real-time step: not engaging, main build id {} (expected {}) base {:#x}",
                    build, MainBuildId, main_base);
    }
    return *matches;
}

bool OotRealtimeStep::Write(Kernel::KProcess* process, float f) {
    auto& memory = process->GetMemory();
    const u64 cell = main_base + TimeObjectCell;
    if (!memory.IsValidVirtualAddress(cell)) {
        return false;
    }
    // Read the pointer every time: the object is created during boot, after the first presents.
    const u64 object = memory.Read64(cell);
    if (object == 0 || !memory.IsValidVirtualAddress(object) ||
        !memory.IsValidVirtualAddress(object + ObjectSize - 1)) {
        return false;
    }
    std::array<u8, ObjectSize> current{};
    memory.ReadBlock(object, current.data(), current.size());
    float one;
    s32 rate;
    std::memcpy(&one, current.data() + OffsetOne, sizeof(one));
    std::memcpy(&rate, current.data() + OffsetRate, sizeof(rate));
    if (one != 1.0f || rate != 60) {
        if (bad_object_logs++ < 3) {
            LOG_WARNING(Service,
                        "OoT real-time step: object {:#x} holds +0x14={} +0x1c={} (expected 1.0, "
                        "60); not writing",
                        object, one, rate);
        }
        return false;
    }
    // Plain data stores through the process's memory, as the cheat engine's writes go (the JIT
    // reads guest data through the same page table / fastmem, so the next guest load sees them).
    // No instruction-cache invalidation: these are data pages.
    const Block block = DeriveBlock(f);
    memory.WriteBlock(object + 0x00, block.main.data(), sizeof(block.main));
    memory.Write32(object + 0x10, std::bit_cast<u32>(block.base));
    memory.Write32(object + 0x18, std::bit_cast<u32>(block.step_seconds));
    if (!engaged_logged) {
        engaged_logged = true;
        LOG_INFO(Service,
                 "OoT real-time step engaged: title {:016X} main {:#x} object {:#x} first f={:.4f} "
                 "({:.1f} presents/s measured)",
                 ProgramId, main_base, object, f, controller.MeasuredFps());
    } else {
        LOG_DEBUG(Service, "OoT real-time step: f={:.4f} step={:.6f} s ({:.1f} presents/s)", f,
                  block.step_seconds, controller.MeasuredFps());
    }
    return true;
}

void OotRealtimeStep::OnPresent(Core::System& system, Kernel::KProcess* process,
                                Clock::time_point now) {
    if (!Enabled()) {
        if (written_process != nullptr) {
            Restore(system, process);
        }
        return;
    }
    if (!Matches(system, process)) {
        return;
    }
    if (written_process != nullptr && written_process != process) {
        // A different process (relaunch): its object starts at f = 1 again.
        written_process = nullptr;
        controller.Reset();
    }
    if (const auto f = controller.OnPresent(now)) {
        if (Write(process, *f)) {
            written_process = *f == 1.0f ? nullptr : process;
        } else {
            // Not written (no object yet): try again on the next present with the same f.
            controller.Reset();
        }
    }
}

void OotRealtimeStep::Restore(Core::System& system, Kernel::KProcess* process) {
    if (written_process == nullptr) {
        return;
    }
    // Only while the process that holds the writes is still the running application.
    if (process == written_process && system.ApplicationProcess() == written_process &&
        Write(process, 1.0f)) {
        LOG_INFO(Service, "OoT real-time step: restored f=1 (step 1/60 s)");
    }
    written_process = nullptr;
    controller.Reset();
}

} // namespace Service::Nx2RealtimeStep
