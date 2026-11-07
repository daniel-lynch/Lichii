// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <memory>

#include <catch2/catch_test_macros.hpp>

#include "audio_core/adsp/apps/audio_renderer/command_list_processor.h"
#include "audio_core/nx2_audio_options.h"
#include "audio_core/renderer/command/resample/downmix_6ch_to_2ch.h"
#include "audio_core/renderer/command/sink/device.h"
#include "tests/audio_core/scoped_environment.h"
#include "tests/audio_core/sysmix_test_context.h"

TEST_CASE("stereo output retains center dialogue and maps all surround channels",
          "[sysmix][nx2_audio_output]") {
    AudioTests::ScopedEnvironment center{"EDEN_NX2_AUDIO_CENTER_GAIN", nullptr};
    SysmixTests::Context context;
    auto stream = std::make_unique<SysmixTests::Stream>(
        context.system, AudioCore::Sink::StreamType::Render, 2, true);
    stream->SetSystemChannels(6);

    constexpr std::array<std::array<s16, 2>, 6> expected{{
        {10000, 0}, {0, 10000}, {5960, 5960}, {3540, 3540}, {7070, 0}, {0, 7070},
    }};
    for (std::size_t channel = 0; channel < 6; ++channel) {
        CAPTURE(channel);
        std::array<s16, AudioCore::TargetSampleCount * 6> samples{};
        for (std::size_t frame = 0; frame < AudioCore::TargetSampleCount; ++frame) {
            samples[frame * 6 + channel] = 10000;
        }
        AudioCore::Sink::SinkBuffer block{AudioCore::TargetSampleCount, 0, 0, false};
        stream->AppendBuffer(block, samples);
        std::array<s16, AudioCore::TargetSampleCount * 2> output{};
        stream->ProcessAudioOutAndRender(output, AudioCore::TargetSampleCount);
        for (std::size_t frame = 0; frame < AudioCore::TargetSampleCount; ++frame) {
            REQUIRE(output[frame * 2] == expected[channel][0]);
            REQUIRE(output[frame * 2 + 1] == expected[channel][1]);
        }
    }
}

TEST_CASE("stereo output saturates summed channels instead of wrapping",
          "[sysmix][nx2_audio_output]") {
    AudioTests::ScopedEnvironment center{"EDEN_NX2_AUDIO_CENTER_GAIN", nullptr};
    SysmixTests::Context context;
    auto stream = std::make_unique<SysmixTests::Stream>(
        context.system, AudioCore::Sink::StreamType::Render, 2, true);
    stream->SetSystemChannels(6);

    for (const s16 sample : {s16{32767}, s16{-32768}}) {
        CAPTURE(sample);
        std::array<s16, AudioCore::TargetSampleCount * 6> samples{};
        samples.fill(sample);
        AudioCore::Sink::SinkBuffer block{AudioCore::TargetSampleCount, 0, 0, false};
        stream->AppendBuffer(block, samples);
        std::array<s16, AudioCore::TargetSampleCount * 2> output{};
        stream->ProcessAudioOutAndRender(output, AudioCore::TargetSampleCount);
        REQUIRE(std::ranges::all_of(output, [sample](s16 value) { return value == sample; }));
    }
}

TEST_CASE("guest downmix coefficients survive the stereo sink without double attenuation",
          "[sysmix][nx2_audio_output]") {
    AudioTests::ScopedEnvironment center{"EDEN_NX2_AUDIO_CENTER_GAIN", nullptr};
    SysmixTests::Context context;
    auto stream = std::make_unique<SysmixTests::Stream>(
        context.system, AudioCore::Sink::StreamType::Render, 2, true);
    std::array<s32, AudioCore::TargetSampleCount * 6> mix_buffers{};
    constexpr std::array<s32, 6> channel_samples{1000, 2000, 4000, 800, 1600, 3200};
    for (std::size_t channel = 0; channel < 6; ++channel) {
        std::fill_n(mix_buffers.begin() + channel * AudioCore::TargetSampleCount,
                    AudioCore::TargetSampleCount, channel_samples[channel]);
    }

    AudioCore::ADSP::AudioRenderer::CommandListProcessor processor;
    processor.stream = stream.get();
    processor.sample_count = AudioCore::TargetSampleCount;
    processor.mix_buffers = mix_buffers;
    processor.buffer_count = 6;

    AudioCore::Renderer::DownMix6chTo2chCommand downmix{};
    downmix.inputs = {0, 1, 2, 3, 4, 5};
    downmix.outputs = downmix.inputs;
    downmix.down_mix_coeff = {1.0f, 0.75f, 0.25f, 0.5f};
    downmix.Process(processor);

    AudioCore::Renderer::DeviceSinkCommand sink{};
    sink.sample_buffer = mix_buffers;
    sink.input_count = 6;
    sink.inputs = downmix.outputs;
    sink.Process(processor);

    std::array<s16, AudioCore::TargetSampleCount * 2> output{};
    stream->ProcessAudioOutAndRender(output, AudioCore::TargetSampleCount);
    for (std::size_t frame = 0; frame < AudioCore::TargetSampleCount; ++frame) {
        REQUIRE(output[frame * 2] == 5000);
        REQUIRE(output[frame * 2 + 1] == 6800);
        for (std::size_t channel = 2; channel < 6; ++channel) {
            REQUIRE(mix_buffers[channel * AudioCore::TargetSampleCount + frame] == 0);
        }
    }
}

TEST_CASE("fallback center gain is bounded and rejects invalid input", "[nx2_audio_output]") {
    struct Case { const char* value; float expected; };
    for (const auto& [value, expected] : std::array<Case, 12>{{
             {nullptr, 0.596f}, {"0.596", 0.596f}, {"0.707", 0.707f}, {"1.0", 1.0f},
             {"0", 0.0f}, {"-1", 0.0f}, {"3", 2.0f}, {"", 0.596f},
             {"garbage", 0.596f}, {"1junk", 0.596f}, {"nan", 0.596f}, {"inf", 0.596f}}}) {
        CAPTURE(value ? value : "unset");
        AudioTests::ScopedEnvironment center{"EDEN_NX2_AUDIO_CENTER_GAIN", value};
        REQUIRE(AudioCore::Nx2AudioOptions::CenterGain() == expected);
    }
}

TEST_CASE("center gain changes only fallback center samples", "[sysmix][nx2_audio_output]") {
    AudioTests::ScopedEnvironment stereo{"EDEN_NX2_AUDIO_STEREO", nullptr};
    SysmixTests::Context context;
    for (const char* value : {"0.596", "0.707", "1.0", "-1", "3"}) {
        CAPTURE(value);
        AudioTests::ScopedEnvironment center{"EDEN_NX2_AUDIO_CENTER_GAIN", value};
        const float gain = AudioCore::Nx2AudioOptions::CenterGain();
        for (u32 host_channels : {2u, 6u}) {
            CAPTURE(host_channels);
            auto stream = std::make_unique<SysmixTests::Stream>(
                context.system, AudioCore::Sink::StreamType::Render, host_channels, true);
            stream->SetSystemChannels(6);
            std::array<s16, AudioCore::TargetSampleCount * 6> samples{};
            for (size_t frame = 0; frame < AudioCore::TargetSampleCount; ++frame) {
                samples[frame * 6] = 1000;
                samples[frame * 6 + 1] = 2000;
                samples[frame * 6 + 2] = 10000;
            }
            AudioCore::Sink::SinkBuffer block{AudioCore::TargetSampleCount, 0, 0, false};
            stream->AppendBuffer(block, samples);
            std::vector<s16> output(AudioCore::TargetSampleCount * host_channels);
            stream->ProcessAudioOutAndRender(output, AudioCore::TargetSampleCount);
            for (size_t frame = 0; frame < AudioCore::TargetSampleCount; ++frame) {
                if (host_channels == 2) {
                    REQUIRE(output[frame * 2] == static_cast<s16>(1000 + 10000 * gain));
                    REQUIRE(output[frame * 2 + 1] == static_cast<s16>(2000 + 10000 * gain));
                } else {
                    REQUIRE(output[frame * 6] == 1000);
                    REQUIRE(output[frame * 6 + 1] == 2000);
                    REQUIRE(output[frame * 6 + 2] == 10000);
                }
            }
        }
    }
}
