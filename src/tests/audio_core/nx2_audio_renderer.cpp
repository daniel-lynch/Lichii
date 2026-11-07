// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>

#include "audio_core/adsp/apps/audio_renderer/command_list_processor.h"
#include "audio_core/common/audio_renderer_parameter.h"
#include "audio_core/renderer/behavior/behavior_info.h"
#include "audio_core/renderer/command/command_buffer.h"
#include "audio_core/renderer/command/command_generator.h"
#include "audio_core/renderer/command/command_list_header.h"
#include "audio_core/renderer/command/command_processing_time_estimator.h"
#include "audio_core/renderer/effect/effect_context.h"
#include "audio_core/renderer/memory/memory_pool_info.h"
#include "audio_core/renderer/mix/mix_context.h"
#include "audio_core/renderer/mix/mix_info.h"
#include "audio_core/renderer/sink/device_sink_info.h"
#include "audio_core/renderer/sink/sink_context.h"
#include "audio_core/renderer/splitter/splitter_context.h"
#include "audio_core/renderer/voice/voice_context.h"
#include "tests/audio_core/scoped_environment.h"

namespace {
using namespace AudioCore;
using namespace AudioCore::Renderer;

struct GeneratorContext {
    GeneratorContext() {
        voices.Initialize(sorted_voices, voice_infos, resources, cpu_states, dsp_states, 1);
        mixes.Initialize(sorted_mixes, {&mix, 1}, 1, {}, 0, {}, 0, {}, 0);
        mix.buffer_count = 2;
        mix.buffer_offset = 0;
        commands.command_list = storage;
        commands.memory_pool = &pool;
        commands.time_estimator = &estimator;
        commands.behavior = &behavior;
    }

    CommandGenerator Generator() {
        return CommandGenerator{commands, header, render, voices, mixes, effects, sinks,
                                splitters, nullptr};
    }

    template <typename F>
    void Visit(F&& visit) {
        for (size_t offset = 0; offset < commands.size;) {
            auto& command = *reinterpret_cast<ICommand*>(storage.data() + offset);
            REQUIRE(command.size > 0);
            visit(command);
            offset += command.size;
        }
    }

    BehaviorInfo behavior;
    MemoryPoolInfo pool;
    VoiceContext voices;
    MixContext mixes;
    EffectContext effects;
    SinkContext sinks;
    SplitterContext splitters;
    std::array<VoiceInfo, 1> voice_infos{};
    std::array<VoiceInfo*, 1> sorted_voices{};
    std::array<VoiceChannelResource, 6> resources{
        VoiceChannelResource{0}, VoiceChannelResource{1}, VoiceChannelResource{2},
        VoiceChannelResource{3}, VoiceChannelResource{4}, VoiceChannelResource{5}};
    std::array<VoiceState, 6> cpu_states{}, dsp_states{};
    MixInfo mix{{}, 0, behavior};
    std::array<MixInfo*, 1> sorted_mixes{};
    alignas(16) std::array<u8, 16384> storage{};
    CommandBuffer commands;
    CommandProcessingTimeEstimatorVersion1 estimator{TargetSampleCount, 8};
    CommandListHeader header{.buffer_count = 8, .sample_count = TargetSampleCount,
                             .sample_rate = TargetSampleRate};
    AudioRendererSystemContext render{.channels = 2, .mix_buffer_count = 2,
                                      .behavior = &behavior, .memory_pool_info = &pool};
};
} // namespace

TEST_CASE("multichannel voice ramps every channel only with the ramp fix",
          "[audio_core][nx2_audio_ramp]") {
    for (const char* value : std::array<const char*, 3>{nullptr, "0", "1"}) {
        CAPTURE(value ? value : "unset");
        AudioTests::ScopedEnvironment option{"EDEN_NX2_AUDIO_RAMP_FIX", value};
        const bool fixed = value && std::string_view{value} == "1";
        for (s8 channels : {s8{2}, s8{6}}) {
            CAPTURE(channels);
            GeneratorContext context;
            auto& voice = context.voice_infos[0];
            voice.channel_count = channels;
            voice.channel_resource_ids = {0, 1, 2, 3, 4, 5};
            voice.sample_format = SampleFormat::PcmInt16;
            voice.sample_rate = TargetSampleRate;
            voice.pitch = 1.0f;
            voice.mix_id = 0;
            voice.prev_volume = 0.0f;
            voice.volume = 1.0f;
            auto generator = context.Generator();
            generator.GenerateVoiceCommand(voice);
            REQUIRE(voice.prev_volume == 1.0f);

            std::array<s32, TargetSampleCount * 8> samples;
            samples.fill(10000);
            ADSP::AudioRenderer::CommandListProcessor processor;
            processor.sample_count = TargetSampleCount;
            processor.mix_buffers = samples;
            processor.buffer_count = 8;
            u32 ramps{};
            context.Visit([&](ICommand& command) {
                if (command.type != CommandId::VolumeRamp) {
                    return;
                }
                auto& ramp = static_cast<VolumeRampCommand&>(command);
                const bool should_ramp = fixed || ramps == 0;
                REQUIRE(ramp.prev_volume == (should_ramp ? 0.0f : 1.0f));
                REQUIRE(ramp.volume == 1.0f);
                ramp.Process(processor);
                const auto output = processor.mix_buffers.subspan(
                    ramp.output_index * TargetSampleCount, TargetSampleCount);
                if (should_ramp) {
                    REQUIRE(output.front() == 0);
                    REQUIRE(output.back() > 9800);
                    REQUIRE(output.back() < 10000);
                    REQUIRE(std::is_sorted(output.begin(), output.end()));
                } else {
                    REQUIRE(std::ranges::all_of(output, [](s32 sample) { return sample == 10000; }));
                }
                ++ramps;
            });
            REQUIRE(ramps == static_cast<u32>(channels));

            context.commands.size = 0;
            context.commands.count = 0;
            generator.GenerateVoiceCommand(voice);
            context.Visit([](ICommand& command) {
                if (command.type == CommandId::VolumeRamp) {
                    const auto& ramp = static_cast<VolumeRampCommand&>(command);
                    REQUIRE(ramp.prev_volume == 1.0f);
                    REQUIRE(ramp.volume == 1.0f);
                }
            });
        }
    }
}

TEST_CASE("renderer emits guest downmix only for stereo with guest downmix enabled",
          "[audio_core][nx2_audio_output]") {
    for (s8 channels : {s8{2}, s8{6}}) {
        for (bool enabled : {false, true}) {
            CAPTURE(channels, enabled);
            GeneratorContext context;
            context.render.channels = channels;
            DeviceSinkInfo sink;
            auto& parameter =
                *reinterpret_cast<DeviceSinkInfo::DeviceInParameter*>(sink.GetParameter());
            parameter.input_count = 6;
            parameter.inputs = {0, 1, 2, 3, 4, 5};
            parameter.downmix_enabled = enabled;
            parameter.downmix_coeff = {1.0f, 0.75f, 0.25f, 0.5f};
            auto generator = context.Generator();
            generator.GenerateDeviceSinkCommand(0, sink);
            u32 downmixes{}, device_sinks{};
            context.Visit([&](ICommand& command) {
                if (command.type == CommandId::DownMix6chTo2ch) {
                    REQUIRE(device_sinks == 0);
                    ++downmixes;
                    const auto& downmix = static_cast<DownMix6chTo2chCommand&>(command);
                    REQUIRE(downmix.down_mix_coeff[1].to_float() == 0.75f);
                } else if (command.type == CommandId::DeviceSink) {
                    ++device_sinks;
                }
            });
            REQUIRE(downmixes == (channels == 2 && enabled ? 1 : 0));
            REQUIRE(device_sinks == 1);
        }
    }
}
