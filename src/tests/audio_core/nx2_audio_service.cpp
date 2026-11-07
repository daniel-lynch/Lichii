// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <catch2/catch_test_macros.hpp>

#include "audio_core/nx2_audio_options.h"
#include "common/scope_exit.h"
#include "core/hle/service/audio/audio_controller.h"
#include "core/hle/service/audio/nx2_audio_user_client.h"
#include "core/hle/service/server_manager.h"
#include "core/hle/service/set/system_settings_server.h"
#include "tests/audio_core/scoped_environment.h"
#include "tests/audio_core/sysmix_test_context.h"

using AudioTests::ScopedEnvironment;

TEST_CASE("audio experiments have explicit defaults and zero opt-outs", "[sysmix][nx2_audio]") {
    using namespace AudioCore::Nx2AudioOptions;
    for (const char* value : std::array<const char*, 5>{nullptr, "", "0", "1", "false"}) {
        CAPTURE(value ? value : "unset");
        ScopedEnvironment shared{"EDEN_NX2_AUDIO_SHARED_MANAGERS", value};
        ScopedEnvironment async{"EDEN_NX2_AUDIO_ASYNC_POOL", value};
        ScopedEnvironment stereo{"EDEN_NX2_AUDIO_STEREO", value};
        ScopedEnvironment ramp{"EDEN_NX2_AUDIO_RAMP_FIX", value};
        const bool explicit_zero = value && std::string_view{value} == "0";
        REQUIRE(SharedManagersEnabled() == !explicit_zero);
        REQUIRE(AsyncMemoryPoolControlEnabled() == !explicit_zero);
        REQUIRE(StereoOutputEnabled() == (value && std::string_view{value} == "1"));
        REQUIRE(RampFixEnabled() == (value && std::string_view{value} == "1"));
    }
}

TEST_CASE("nx2 audio IPC returns registered managers across repeated requests",
          "[sysmix][nx2_audio]") {
    bool share = true;
    SECTION("default shares registered services") {}
    SECTION("zero restores private managers") { share = false; }
    ScopedEnvironment option{"EDEN_NX2_AUDIO_SHARED_MANAGERS", share ? nullptr : "0"};
    SysmixTests::Context context;
    // Dummy threads use TLS address zero; provide backing for the IPC response copy.
    context.process->GetPageTable().GetImpl().entries.GetAndFault(0).Store(
        false, Common::PageType::Memory, 1, reinterpret_cast<uintptr_t>(context.backing));
    auto& kernel = context.system.Kernel();
    Kernel::KThread client{kernel};
    Kernel::KAutoObject::Create(&client);
    REQUIRE(Kernel::KThread::InitializeDummyThread(context.system, &client, &*context.process)
                .IsSuccess());
    SCOPE_EXIT {
        context.process->UnregisterThread(kernel, &client);
        context.process->Close(kernel);
        kernel.UnregisterKernelObject(&client);
    };
    std::optional<std::jthread> server_thread;
    Service::ServerManager server{context.system};
    server_thread.emplace(
        kernel.RunOnHostCoreProcess("nx2-audio-ipc-test", [&] { server.LoopProcess(); }));
    Service::SM::ServiceManager registry{kernel};
    const auto input = std::make_shared<Service::Audio::IAudioInManager>(context.system);
    const auto output = std::make_shared<Service::Audio::IAudioOutManager>(context.system);
    Kernel::KServerPort* input_port{};
    Kernel::KServerPort* output_port{};
    REQUIRE(registry.RegisterService(&input_port, "audin:u", 30, [input] { return input; })
                .IsSuccess());
    SCOPE_EXIT { input_port->Close(kernel); };
    REQUIRE(registry.RegisterService(&output_port, "audout:u", 30, [output] { return output; })
                .IsSuccess());
    SCOPE_EXIT { output_port->Close(kernel); };

    auto service = std::make_shared<Service::Audio::INx2AudioUserClient>(context.system, registry);
    auto manager = std::make_shared<Service::SessionRequestManager>(kernel, server);
    manager->SetSessionHandler(service);
    manager->ConvertToDomain();
    const auto query = [&](u32 command) {
        std::array<u32_le, IPC::COMMAND_BUFFER_LENGTH> words{};
        IPC::CommandHeader header{};
        header.type.Assign(IPC::CommandType::Request);
        header.data_size.Assign(16);
        std::memcpy(words.data(), &header, sizeof(header));
        IPC::DomainMessageHeader domain{};
        domain.command.Assign(IPC::DomainMessageHeader::CommandType::SendMessage);
        domain.size.Assign(20);
        domain.object_id = 1;
        std::memcpy(words.data() + 4, &domain, sizeof(domain));
        words[8] = 0x49434653;
        words[10] = command;
        Service::HLERequestContext request{kernel, context.Memory(), nullptr, &client};
        request.SetSessionRequestManager(manager);
        REQUIRE(request.PopulateFromIncomingCommandBuffer(words.data()).IsSuccess());
        service->InvokeRequest(request);
        REQUIRE(request.CommandBuffer()[8] == 0x4f434653);
        const auto result = request.CommandBuffer()[10];
        REQUIRE(request.WriteToOutgoingCommandBuffer().IsSuccess());
        return result;
    };

    for (u32 repeat = 0; repeat < 2; ++repeat) {
        for (u32 command = 0; command < 2; ++command) {
            REQUIRE(query(command) == ResultSuccess.raw);
            const auto returned = manager->DomainHandler(manager->DomainHandlerCount() - 1).lock();
            const Service::SessionRequestHandler* expected = command == 0
                ? static_cast<Service::SessionRequestHandler*>(input.get())
                : static_cast<Service::SessionRequestHandler*>(output.get());
            REQUIRE((returned.get() == expected) == share);
            if (repeat != 0) {
                REQUIRE((returned == manager->DomainHandler(command + 1).lock()) == share);
            }
        }
    }
    if (share) {
        const auto count = manager->DomainHandlerCount();
        REQUIRE(query(2) == Service::Audio::ResultOperationFailed.raw);
        REQUIRE(manager->DomainHandlerCount() == count);
    }
}

TEST_CASE("audio output mode IPC keeps session writes and stereo overrides both query paths",
          "[sysmix][nx2_audio_output]") {
    using Mode = Service::Set::AudioOutputMode;
    using Target = Service::Set::AudioOutputModeTarget;
    ScopedEnvironment stereo{"EDEN_NX2_AUDIO_STEREO", nullptr};
    SysmixTests::Context context;
    // No real output sink is initialized by this service-only fixture.
    Settings::values.audio_muted.SetValue(true);
    auto& kernel = context.system.Kernel();
    Kernel::KThread client{kernel};
    Kernel::KAutoObject::Create(&client);
    REQUIRE(Kernel::KThread::InitializeDummyThread(context.system, &client, &*context.process)
                .IsSuccess());
    SCOPE_EXIT {
        context.process->UnregisterThread(kernel, &client);
        context.process->Close(kernel);
        kernel.UnregisterKernelObject(&client);
    };
    std::optional<std::jthread> server_thread;
    Service::ServerManager server{context.system};
    server_thread.emplace(
        kernel.RunOnHostCoreProcess("audio-mode-ipc-test", [&] { server.LoopProcess(); }));
    auto manager = std::make_shared<Service::SessionRequestManager>(kernel, server);
    auto settings = std::make_shared<Service::Set::ISystemSettingsServer>(context.system);
    Service::Audio::IAudioController controller{context.system, settings};
    const auto query = [&](u32 command, Target target, Mode value = Mode::ch_7_1) {
        std::array<u32_le, IPC::COMMAND_BUFFER_LENGTH> words{};
        IPC::CommandHeader header{};
        header.type.Assign(IPC::CommandType::Request);
        header.data_size.Assign(12);
        std::memcpy(words.data(), &header, sizeof(header));
        words[4] = 0x49434653;
        words[6] = command;
        words[8] = static_cast<u32>(target);
        words[9] = static_cast<u32>(value);
        Service::HLERequestContext request{kernel, context.Memory(), nullptr, &client};
        request.SetSessionRequestManager(manager);
        REQUIRE(request.PopulateFromIncomingCommandBuffer(words.data()).IsSuccess());
        controller.InvokeRequest(request);
        REQUIRE(request.CommandBuffer()[4] == 0x4f434653);
        REQUIRE(request.CommandBuffer()[6] == ResultSuccess.raw);
        return static_cast<Mode>(static_cast<u32>(request.CommandBuffer()[8]));
    };
    constexpr std::array targets{Target::Hdmi, Target::Speaker, Target::Headphone,
                                 Target::Type3, Target::Type4};
    for (auto target : targets) {
        REQUIRE(query(13, target) == Mode::ch_7_1);
        query(14, target, Mode::ch_5_1);
        REQUIRE(query(13, target) == Mode::ch_5_1);
        query(10, target, Mode::ch_1);
        REQUIRE(query(9, target) == Mode::ch_1);
    }
    query(14, Target::Hdmi, Mode::ch_2);
    REQUIRE(query(13, Target::Speaker) == Mode::ch_5_1);
    for (const char* option : {"0", "1"}) {
        ScopedEnvironment override{"EDEN_NX2_AUDIO_STEREO", option};
        const bool enabled = std::string_view{option} == "1";
        for (auto target : targets) {
            CAPTURE(option, static_cast<u32>(target));
            query(14, target, enabled ? Mode::ch_1 : Mode::ch_7_1);
            query(10, target, Mode::ch_5_1);
            REQUIRE(query(13, target) == (enabled ? Mode::ch_2 : Mode::ch_7_1));
            REQUIRE(query(9, target) == (enabled ? Mode::ch_2 : Mode::ch_5_1));
            Mode mode{};
            REQUIRE(settings->GetAudioOutputMode(&mode, target).IsSuccess());
            REQUIRE(mode == (enabled ? Mode::ch_2 : Mode::ch_5_1));
        }
    }
    for (auto target : targets) {
        REQUIRE(query(13, target) == Mode::ch_1);
        REQUIRE(query(9, target) == Mode::ch_5_1);
    }
}
