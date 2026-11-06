// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <fmt/format.h>
#include "common/logging.h"
#include "video_core/nx2_debug_control.h"
#include "video_core/nx2_flash_trigger.h"

namespace VideoCore::Nx2Debug {
namespace {
std::mutex mutex;
FlashStatus status;
std::atomic<bool> enabled{};
bool trigger_enabled{};
u64 generation{};
u64 capture_id{u64{1} << 63};
MonitorStatus monitor;
std::optional<FlickerMeter> meter;
u64 monitor_generation{};

void Update() {
    if (status.config.generation && std::chrono::steady_clock::now() >= status.config.deadline) {
        if (status.armed) {
            status.reason = "timeout";
        }
        status.armed = false;
        for (auto& capture : status.captures) {
            if (!capture.capture_white_fraction && capture.error.empty()) {
                capture.error = "captured frame measurement timed out";
            }
        }
    }
    const bool awaiting_sample = std::ranges::any_of(status.captures, [](const auto& capture) {
        return !capture.capture_white_fraction && capture.error.empty();
    });
    trigger_enabled = status.armed || awaiting_sample;
    enabled.store(trigger_enabled || monitor.enabled, std::memory_order_relaxed);
}

void Count(FlickerCounters& counters, const FrameVerdict& verdict) {
    ++counters.judged;
    counters.max_jump = (std::max)(counters.max_jump, verdict.flash_jump);
    counters.max_tile_jump = (std::max)(counters.max_tile_jump, verdict.tile_jump);
    if (verdict.flash) {
        ++counters.flashes;
        counters.last_flash_frame = verdict.frame;
    }
    if (verdict.flicker) {
        ++counters.flicker_frames;
        counters.flicker_tiles += verdict.flicker_tiles;
        counters.last_flicker_frame = verdict.frame;
    }
}

void CopyMeterCounts() {
    if (meter) {
        // The meter's own counters are cumulative; the window gets the delta since its start.
        const u64 duplicates = meter->Duplicates() - monitor.total.duplicates;
        const u64 gaps = meter->Gaps() - monitor.total.gaps;
        monitor.window.duplicates += duplicates;
        monitor.window.gaps += gaps;
        monitor.total.duplicates += duplicates;
        monitor.total.gaps += gaps;
    }
}
}

double WhiteFraction(std::span<const u8> rgba, double min_luma) {
    if (rgba.empty() || rgba.size() % 4 != 0) {
        return 0;
    }
    size_t white{};
    const double threshold = min_luma * 255.0 * 10000.0;
    for (size_t i = 0; i < rgba.size(); i += 4) {
        white += 2126 * rgba[i] + 7152 * rgba[i + 1] + 722 * rgba[i + 2] >= threshold;
    }
    return static_cast<double>(white) / static_cast<double>(rgba.size() / 4);
}

FlashStatus ArmFlash(FlashConfig config) {
    std::scoped_lock lock{mutex};
    if (status.armed || std::ranges::any_of(status.captures, [](const auto& capture) {
            return !capture.complete;
        })) {
        throw std::runtime_error{"flash trigger or its dump is still active; disarm first and wait for the dump"};
    }
    config.generation = ++generation;
    status = {};
    status.config = std::move(config);
    status.armed = true;
    Update();
    return status;
}

FlashStatus DisarmFlash() {
    std::scoped_lock lock{mutex};
    status.armed = false;
    status.reason = "disarmed";
    for (auto& capture : status.captures) {
        if (!capture.capture_white_fraction && capture.error.empty()) {
            capture.error = "disarmed before capture-frame measurement";
        }
    }
    Update();
    return status;
}

void ResetFlash() {
    std::scoped_lock lock{mutex};
    ++generation;
    status = {};
    monitor = {};
    meter.reset();
    trigger_enabled = false;
    enabled.store(false, std::memory_order_relaxed);
}

FlashStatus ReadFlashStatus() {
    std::scoped_lock lock{mutex};
    Update();
    return status;
}

bool FlashEnabled() noexcept {
    return enabled.load(std::memory_order_relaxed);
}

std::optional<FlashConfig> GetFlashConfig() {
    std::scoped_lock lock{mutex};
    Update();
    // The white-fraction trigger only; the flicker monitor also sets `enabled`.
    return trigger_enabled ? std::optional{status.config} : std::nullopt;
}

void ObserveFlash(u64 session, u64 source_frame, u64 current_frame, double fraction) {
    std::optional<CaptureRequest> request;
    {
        std::scoped_lock lock{mutex};
        if (session != status.config.generation) {
            return;
        }
        Update();
        ++status.samples;
        for (auto& capture : status.captures) {
            if (capture.capture_frame == source_frame) {
                capture.capture_white_fraction = fraction;
            }
        }
        const bool busy = std::ranges::any_of(status.captures, [](const auto& capture) {
            return !capture.complete || (!capture.capture_white_fraction && capture.error.empty());
        });
        const bool fresh = status.captures.empty() || source_frame > status.captures.back().capture_frame;
        if (status.armed && !busy && fresh && fraction >= status.config.threshold) {
            const u64 id = ++capture_id;
            const auto directory = status.config.directory /
                                   fmt::format("capture-{:03}", status.captures.size() + 1);
            status.captures.push_back({.id = id, .directory = directory,
                                      .trigger_frame = source_frame,
                                      .capture_frame = current_frame + 1,
                                      .trigger_white_fraction = fraction});
            request = CaptureRequest{.id = id, .directory = directory,
                                     .sampled = status.config.sampled,
                                     .buffers = status.config.buffers};
            if (status.captures.size() >= status.config.max_captures) {
                status.armed = false;
                status.reason = "capture_limit";
            }
        }
        Update();
    }
    if (request) {
        RequestCapture(std::move(*request));
    }
}

void NoteFlashCost(u64 session, double cpu_ms, std::optional<double> gpu_ms, bool dropped) {
    std::scoped_lock lock{mutex};
    if (session != status.config.generation) {
        return;
    }
    if (cpu_ms >= 0) {
        ++status.cpu_checks;
        status.cpu_total_ms += cpu_ms;
        status.cpu_max_ms = (std::max)(status.cpu_max_ms, cpu_ms);
    }
    if (gpu_ms) {
        ++status.gpu_checks;
        status.gpu_total_ms += *gpu_ms;
        status.gpu_max_ms = (std::max)(status.gpu_max_ms, *gpu_ms);
    }
    status.dropped += dropped;
}

bool FinishFlashCapture(u64 id, const std::string& error) {
    if (!(id & (u64{1} << 63))) {
        return false;
    }
    std::scoped_lock lock{mutex};
    if (monitor.capture && monitor.capture->id == id) {
        monitor.capture->complete = true;
        monitor.capture->error = error;
    }
    for (auto& capture : status.captures) {
        if (capture.id == id) {
            capture.complete = true;
            if (!error.empty()) {
                capture.error = error;
            }
        }
    }
    Update();
    return true;
}

void FailFlash(u64 session, std::string error) {
    std::scoped_lock lock{mutex};
    if (session != status.config.generation) {
        return;
    }
    status.armed = false;
    status.reason = error;
    for (auto& capture : status.captures) {
        if (!capture.capture_white_fraction) {
            capture.error = error;
        }
    }
    Update();
}

void DropFlashSample(u64 session, u64 frame) {
    std::scoped_lock lock{mutex};
    if (session != status.config.generation) {
        return;
    }
    for (auto& capture : status.captures) {
        if (capture.capture_frame == frame) {
            capture.error = "captured frame probe dropped: GPU readback ring full";
        }
    }
    Update();
}

MonitorStatus StartMonitor(MonitorConfig config) {
    std::scoped_lock lock{mutex};
    if (config.thresholds.flash_jump <= 0 || config.thresholds.tile_jump <= 0) {
        throw std::runtime_error{"flash_jump and tile_jump must be positive"};
    }
    if (monitor.capture && !monitor.capture->complete) {
        throw std::runtime_error{"the previous monitor's first-flash dump is still being written"};
    }
    monitor = {};
    monitor.enabled = true;
    monitor.generation = ++monitor_generation;
    monitor.config = std::move(config);
    meter.emplace(monitor.config.thresholds);
    Update();
    return monitor;
}

MonitorStatus StopMonitor() {
    std::scoped_lock lock{mutex};
    monitor.enabled = false;
    Update();
    return monitor;
}

MonitorStatus ReadMonitor(bool reset_window) {
    std::scoped_lock lock{mutex};
    CopyMeterCounts();
    MonitorStatus out = monitor;
    if (reset_window) {
        monitor.window = {};
    }
    return out;
}

std::optional<u64> MonitorGeneration() {
    std::scoped_lock lock{mutex};
    return monitor.enabled ? std::optional{monitor.generation} : std::nullopt;
}

void ObserveMonitor(u64 session, u64 frame, u64 current_frame, std::span<const u8> rgba) {
    const auto features = ComputeFeatures(rgba);
    std::optional<CaptureRequest> request;
    {
        std::scoped_lock lock{mutex};
        if (!features || !meter || session != monitor.generation) {
            return;
        }
        ++monitor.window.frames;
        ++monitor.total.frames;
        monitor.window.luma_sum += features->centre_mean;
        monitor.total.luma_sum += features->centre_mean;
        const auto verdict = meter->Push(frame, *features);
        CopyMeterCounts();
        if (!verdict) {
            return;
        }
        Count(monitor.window, *verdict);
        Count(monitor.total, *verdict);
        if (verdict->flash && monitor.total.flashes <= 50) {
            LOG_INFO(Render_Vulkan, "NX2 flash monitor: flash at probe frame {} (+{:.1f} luma over both neighbours)",
                     verdict->frame, verdict->flash_jump);
        }
        if (verdict->flash && monitor.config.capture_first && !monitor.capture) {
            const u64 id = ++capture_id;
            const auto directory = monitor.config.directory / "first-flash";
            monitor.capture = MonitorCapture{.id = id, .directory = directory,
                                             .trigger_frame = verdict->frame,
                                             .capture_frame = current_frame + 1,
                                             .flash_jump = verdict->flash_jump};
            request = CaptureRequest{.id = id, .directory = directory,
                                     .sampled = monitor.config.sampled};
        }
    }
    if (request) {
        RequestCapture(std::move(*request)); // the frame dumper creates the directory
    }
}

void NoteMonitorCost(u64 session, double cpu_ms, std::optional<double> gpu_ms, bool dropped) {
    std::scoped_lock lock{mutex};
    if (session != monitor.generation) {
        return;
    }
    if (cpu_ms >= 0) {
        ++monitor.cpu_checks;
        monitor.cpu_total_ms += cpu_ms;
        monitor.cpu_max_ms = (std::max)(monitor.cpu_max_ms, cpu_ms);
    }
    if (gpu_ms) {
        ++monitor.gpu_checks;
        monitor.gpu_total_ms += *gpu_ms;
        monitor.gpu_max_ms = (std::max)(monitor.gpu_max_ms, *gpu_ms);
    }
    monitor.dropped += dropped;
}

} // namespace VideoCore::Nx2Debug
