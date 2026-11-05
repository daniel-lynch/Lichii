// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include "common/common_types.h"
#include "video_core/nx2_flicker_meter.h"

namespace VideoCore::Nx2Debug {

struct FlashConfig {
    u64 generation{};
    std::filesystem::path directory;
    double threshold{};
    double min_luma{};
    u32 max_captures{1};
    std::chrono::steady_clock::time_point deadline;
    bool buffers{};
    bool sampled{};
};

struct FlashCapture {
    u64 id{};
    std::filesystem::path directory;
    u64 trigger_frame{};
    u64 capture_frame{};
    double trigger_white_fraction{};
    std::optional<double> capture_white_fraction;
    bool complete{};
    std::string error;
};

struct FlashStatus {
    FlashConfig config;
    bool armed{};
    std::string reason;
    std::vector<FlashCapture> captures;
    u64 samples{};
    u64 dropped{};
    u64 cpu_checks{};
    u64 gpu_checks{};
    double cpu_total_ms{};
    double cpu_max_ms{};
    double gpu_total_ms{};
    double gpu_max_ms{};
};

/// Continuous flash/flicker statistics (flash_monitor / flash_stats). Off unless started.
struct FlickerCounters {
    u64 frames{};          ///< probed presents
    u64 judged{};          ///< distinct frames that got a verdict (both neighbours seen)
    u64 duplicates{};      ///< repeated presents skipped
    u64 gaps{};            ///< dropped probes that restarted the three-frame window
    u64 flashes{};
    u64 flicker_frames{};
    u64 flicker_tiles{};
    double max_jump{};     ///< largest centre one-frame brightening seen (luma 0..255)
    double max_tile_jump{};
    std::optional<u64> last_flash_frame;
    std::optional<u64> last_flicker_frame;
    double luma_sum{};
};

struct MonitorConfig {
    std::filesystem::path directory; ///< where the first-flash capture goes
    FlickerThresholds thresholds;
    bool capture_first{true};
    bool sampled{};
};

struct MonitorCapture {
    u64 id{};
    std::filesystem::path directory;
    u64 trigger_frame{};
    u64 capture_frame{};
    double flash_jump{};
    bool complete{};
    std::string error;
};

struct MonitorStatus {
    bool enabled{};
    u64 generation{};
    MonitorConfig config;
    FlickerCounters window; ///< since the last flash_stats reset
    FlickerCounters total;  ///< since flash_monitor started
    std::optional<MonitorCapture> capture;
    u64 dropped{};
    u64 cpu_checks{};
    u64 gpu_checks{};
    double cpu_total_ms{};
    double cpu_max_ms{};
    double gpu_total_ms{};
    double gpu_max_ms{};
};

MonitorStatus StartMonitor(MonitorConfig config);
MonitorStatus StopMonitor();
/// reset_window: return the window counters and start a new window.
MonitorStatus ReadMonitor(bool reset_window);
/// Render thread: the generation to tag probes with, or nullopt while the monitor is off.
[[nodiscard]] std::optional<u64> MonitorGeneration();
/// Render thread: one completed probe readback, in present order.
void ObserveMonitor(u64 generation, u64 frame, u64 current_frame, std::span<const u8> rgba);
void NoteMonitorCost(u64 generation, double cpu_ms, std::optional<double> gpu_ms, bool dropped);

double WhiteFraction(std::span<const u8> rgba, double min_luma);
FlashStatus ArmFlash(FlashConfig config);
FlashStatus DisarmFlash();
void ResetFlash();
FlashStatus ReadFlashStatus();
bool FlashEnabled() noexcept;
std::optional<FlashConfig> GetFlashConfig();
void ObserveFlash(u64 generation, u64 source_frame, u64 current_frame, double fraction);
void NoteFlashCost(u64 generation, double cpu_ms, std::optional<double> gpu_ms, bool dropped);
bool FinishFlashCapture(u64 id, const std::string& error);
void FailFlash(u64 generation, std::string error);
void DropFlashSample(u64 generation, u64 frame);

} // namespace VideoCore::Nx2Debug
