// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "audio_core/nx2_audio_options.h"
#include "core/hle/service/audio/audio_in_manager.h"
#include "core/hle/service/audio/audio_out_manager.h"
#include "core/hle/service/audio/audio_renderer_manager.h"
#include "core/hle/service/audio/errors.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/service.h"
#include "core/hle/service/sm/sm.h"

namespace Service::Audio {

// Switch 2 "aud:u" (nn::audio::CreateUserClientSessionByHipc). The SDK reaches the Switch 1
// audio interfaces through it: command 2 returns an object used exactly like
// IAudioRendererManager (vtable slot 1 = GetWorkBufferSize with a Switch 1 parameter block), so
// it is backed by Eden's audren:u implementation. Command 1 is nn::audio::OpenAudioOut's
// manager, called like IAudioOutManager (slot 1 = OpenAudioOut), backed by audout:u. The nx2: name
// keeps unknown commands auto-stubbed and logged.
class INx2AudioUserClient final : public ServiceFramework<INx2AudioUserClient> {
public:
    explicit INx2AudioUserClient(Core::System& system_, SM::ServiceManager& service_manager_)
        : ServiceFramework{system_, "nx2:aud:u"}, service_manager{service_manager_},
          shared_managers{AudioCore::Nx2AudioOptions::SharedManagersEnabled()} {
        // clang-format off
        static const FunctionInfo functions[] = {
            {0, &INx2AudioUserClient::GetAudioInManager, "GetAudioInManager"},
            {1, &INx2AudioUserClient::GetAudioOutManager, "GetAudioOutManager (guessed)"},
            {2, &INx2AudioUserClient::GetAudioRendererManager, "GetAudioRendererManager (guessed)"},
        };
        // clang-format on
        RegisterHandlers(functions);
    }

private:
    template <typename T>
    void ReplyManager(HLERequestContext& ctx, const char* name) {
        // AudioManager has one release callback per direction. Private managers cannot share it.
        auto manager = shared_managers ? service_manager.GetService<T>(name)
                                       : std::make_shared<T>(system);
        if (!manager) {
            LOG_ERROR(Service_Audio, "Switch 2 aud:u: {} is not registered", name);
            IPC::ResponseBuilder rb{ctx, 2};
            rb.Push(ResultOperationFailed);
            return;
        }
        LOG_INFO(Service_Audio, "Switch 2 aud:u returning {} {}",
                 shared_managers ? "shared" : "private", name);
        IPC::ResponseBuilder rb{ctx, 2, 0, 1};
        rb.Push(ResultSuccess);
        rb.PushIpcInterface<T>(ctx, std::move(manager));
    }

    void GetAudioInManager(HLERequestContext& ctx) {
        ReplyManager<IAudioInManager>(ctx, "audin:u");
    }

    void GetAudioOutManager(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const u32 arg = rp.Pop<u32>();
        LOG_DEBUG(Service_Audio, "Switch 2 aud:u 1 arg={}", arg);
        ReplyManager<IAudioOutManager>(ctx, "audout:u");
    }

    void GetAudioRendererManager(HLERequestContext& ctx) {
        IPC::RequestParser rp{ctx};
        const u32 arg = rp.Pop<u32>();
        LOG_DEBUG(Service_Audio, "Switch 2 aud:u 2 arg={}", arg);
        ReplyManager<IAudioRendererManager>(ctx, "audren:u");
    }

    SM::ServiceManager& service_manager;
    const bool shared_managers;
};

} // namespace Service::Audio
