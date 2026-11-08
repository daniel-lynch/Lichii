// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "common/scope_exit.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/file_sys/program_metadata.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/memory.h"
#include "tests/core/system_test_context.h"

// The C4 field launch failed here with ResultInvalidCurrentMemory (0xd401): in a 39-bit address
// space the AliasCode region spans the heap and alias regions, so MapPages refused the sentinel.
TEST_CASE("OoT co-op call memory maps a private stack and sentinel in a 39-bit process",
          "[oot_coop]") {
    ScopedSystemTestContext context;
    Core::System system;
    system.Initialize();
    auto& kernel = system.Kernel();
    Kernel::KThread bootstrap{kernel};
    REQUIRE(Kernel::KThread::InitializeDummyThread(system, &bootstrap, nullptr).IsSuccess());
    kernel.SetCurrentEmuThread(&bootstrap);
    kernel.Initialize();
    SCOPE_EXIT {
        system.CoreTiming().ClearPendingEvents();
        kernel.Shutdown();
        kernel.SetCurrentEmuThread(nullptr);
        kernel.UnregisterKernelObject(&bootstrap);
    };
    Kernel::KProcess process_object{kernel};
    Kernel::KAutoObject::Create(&process_object);
    SCOPE_EXIT {
        kernel.UnregisterKernelObject(&process_object);
    };
    auto* process = &process_object;
    // The loader's path: default metadata is a 64-bit, 39-bit address space application.
    const auto loaded =
        process->LoadFromMetadata(kernel, FileSys::ProgramMetadata::GetDefault(), 0x100000, 0, 0);
    CAPTURE(loaded.raw);
    REQUIRE(loaded.IsSuccess());
    SCOPE_EXIT {
        process->Close(kernel);
    };
    Kernel::KProcessAddress stack{}, sentinel{};
    const auto result = process->AllocateDebugCallMemory(kernel, &stack, &sentinel);
    CAPTURE(result.raw);
    REQUIRE(result.IsSuccess());
    auto& memory = process->GetMemory();
    memory.SetCurrentPageTable(*process);
    REQUIRE(GetInteger(stack) % 16 == 0);
    REQUIRE(GetInteger(sentinel) % 0x1000 == 0);
    REQUIRE(memory.IsValidVirtualAddressRange(GetInteger(stack) - 0x4000, 0x4000));
    REQUIRE(memory.IsValidVirtualAddressRange(GetInteger(sentinel), 0x1000));
    REQUIRE(memory.Read32(GetInteger(sentinel)) == 0xD4200000); // brk #0
    // The sentinel is not inside the stack the setter runs on.
    REQUIRE((GetInteger(sentinel) + 0x1000 <= GetInteger(stack) - 0x4000 ||
             GetInteger(sentinel) >= GetInteger(stack)));
    Kernel::KProcessAddress again_stack{}, again_sentinel{};
    REQUIRE(process->AllocateDebugCallMemory(kernel, &again_stack, &again_sentinel).IsError());
}
