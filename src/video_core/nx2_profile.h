// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#ifdef _MSC_VER
#include <intrin.h>
#elif defined(ARCHITECTURE_x86_64)
#include <x86intrin.h>
#endif
#include <chrono>
#include <cstdlib>
#include <initializer_list>
#include <optional>
#include <string>
#include <unordered_set>

#include "common/common_types.h"
#include "video_core/gpu_split.h"
#include "common/logging.h"
#include "common/cpu_features.h"
#include "common/spin_recursive_mutex.h"
#include "common/thread_cpu_clock.h"
#include "video_core/nx2_stutter_trace.h"

// EDEN_NX2_GPU_PROFILE: time where the GPU thread spends its time. Each zone counts its own
// (exclusive) time, nested zones excluded; every 10 s the thread logs each zone's share.
// EDEN_NX2_GPU_WAITS=1: only the wait zones (IsWaitZone) and the back's idle hook are timed, and
// the front and back threads each log one "waits per second" line (FormatWaitsLine); everything
// else on the thread is work. It leaves Enabled() false, so the fast 3D method path, method and
// spin contention counting stay as they are without it.
namespace VideoCommon::Nx2Profile {

enum class Zone : u32 {
    Other, ///< GPU thread time outside every zone below (register writes, command parsing)
    Idle,
    PendingSleep,
    ResumePending,
    Push,
    Tick,
    Run,
    FlushInvalidate,
    Macro,
    Draw,
    Clear,
    Compute,
    FlushWork,
    PipelineLookup,
    Configure,
    SyncDescriptors,
    ReadHandles,
    FillImageViews,
    TextureBuffers,
    UpdateBuffers,
    DescriptorAcquire,
    BindStageBuffers,
    PushDescriptors,
    RenderTargets,
    ConfigureDraw,
    DynamicState,
    DrawRecord,
    TextureUpload,
    Fence,        ///< Fences made (SignalFence)
    FenceSkipped, ///< Empty fences not made
    Method,       ///< Engine method calls (what a split GPU thread's back half would run)
    SplitDrain,   ///< Split GPU thread: the front waits for the back (gpu_split.h)
    SplitWait,    ///< Split GPU thread: the back waits for work
    WriterWait,
    ListSubmitWait,
    ListGpuWait,
    GraphicsPipelineMiss, ///< A graphics pipeline built on the GPU thread
    ComputePipelineMiss,  ///< A compute pipeline built on the GPU thread
    PipelineWait,         ///< VulkanWorker: a draw or dispatch waits for its pipeline's build
    ShaderCapture,        ///< A program the shader cache sees first: captured, lowered, hashed
    TextureCache,         ///< Texture cache downloads, joins, blits and async flushes and decodes
    GpuWait,              ///< Waiting for a GPU tick
    Present,              ///< Swapchain frame acquire and present
    WorkerIdle,           ///< VulkanWorker waiting for work
    PresentWait,          ///< Present waits for the Vulkan worker or a free presentation frame
    QueueSubmit,          ///< vkQueueSubmit
    IdleHook,             ///< Split back: the idle hook (Scheduler::FlushDeferred, a submit)
    Count,
};

inline constexpr std::array<const char*, static_cast<size_t>(Zone::Count)> zone_names{
    "other",         "idle",          "pending sleep",  "resume pending", "push",
    "tick",          "run",           "flush/inval",    "macro",          "draw",
    "clear",         "compute",       "flush work",     "pipeline lookup", "configure",
    "sync descriptors", "read handles", "fill image views", "texture buffers", "update buffers",
    "descriptor acquire", "bind stage buffers", "push descriptors", "render targets",
    "configure draw", "dynamic state", "draw record", "texture upload", "fence",
    "fence skipped", "method", "split drain", "split wait",
    "writer wait", "list submit wait", "list GPU wait", "graphics pipeline miss",
    "compute pipeline miss", "pipeline wait", "shader capture", "texture cache", "GPU wait",
    "present", "worker idle",
    "present wait", "queue submit", "idle hook",
};

static_assert(static_cast<size_t>(Zone::Count) <= StutterTrace::ZoneSlots);

/// The EDEN_NX2_STUTTER_TRACE category of each zone's exclusive time (None: counted as other).
inline constexpr auto stutter_categories = [] {
    using StutterTrace::Category;
    std::array<Category, static_cast<size_t>(Zone::Count)> categories;
    categories.fill(Category::None);
    const auto set = [&categories](Zone zone, Category category) {
        categories[static_cast<size_t>(zone)] = category;
    };
    set(Zone::Idle, Category::Idle);
    set(Zone::PendingSleep, Category::Idle);
    set(Zone::SplitDrain, Category::Idle);
    set(Zone::SplitWait, Category::Idle);
    set(Zone::WriterWait, Category::Idle);
    set(Zone::WorkerIdle, Category::Idle);
    set(Zone::GraphicsPipelineMiss, Category::GraphicsMiss);
    set(Zone::ComputePipelineMiss, Category::ComputeMiss);
    set(Zone::PipelineWait, Category::PipelineWait);
    set(Zone::ShaderCapture, Category::ShaderCapture);
    set(Zone::SyncDescriptors, Category::TextureCache);
    set(Zone::FillImageViews, Category::TextureCache);
    set(Zone::RenderTargets, Category::TextureCache);
    set(Zone::TextureUpload, Category::TextureCache);
    set(Zone::TextureCache, Category::TextureCache);
    set(Zone::TextureBuffers, Category::BufferCache);
    set(Zone::UpdateBuffers, Category::BufferCache);
    set(Zone::BindStageBuffers, Category::BufferCache);
    set(Zone::Fence, Category::QueryFence);
    set(Zone::ListSubmitWait, Category::QueryFence);
    set(Zone::ListGpuWait, Category::QueryFence);
    set(Zone::GpuWait, Category::QueryFence);
    set(Zone::Present, Category::Present);
    set(Zone::PresentWait, Category::Present);
    return categories;
}();

// A plain flag: Enabled() runs several times per method write (no guard to check).
// Off while EDEN_NX2_SAMPLE_PROFILE samples (common/nx2_sample_profile.h): the zone timers would
// show up in the samples.
inline bool g_enabled = std::getenv("EDEN_NX2_GPU_PROFILE") != nullptr &&
                        std::getenv("EDEN_NX2_SAMPLE_PROFILE") == nullptr;

/// EDEN_NX2_GPU_WAITS: set and not "0" (off while EDEN_NX2_SAMPLE_PROFILE samples, as above).
inline bool ReadWaitsEnv() {
    const char* const value = std::getenv("EDEN_NX2_GPU_WAITS");
    return value != nullptr && value[0] != '\0' && value[0] != '0' &&
           std::getenv("EDEN_NX2_SAMPLE_PROFILE") == nullptr;
}
/// EDEN_NX2_GPU_WAITS: time the wait zones only and log the waits lines.
inline bool g_waits = ReadWaitsEnv();
/// Whether every zone measures: for the profile or for EDEN_NX2_STUTTER_TRACE.
inline bool g_zones_enabled = g_enabled || StutterTrace::ReadEnv();
/// Whether the wait zones (IsWaitZone) measure: any of the above or EDEN_NX2_GPU_WAITS.
inline bool g_wait_zones_enabled = g_zones_enabled || g_waits;
/// Whether Report has anything to log and AddDrain counts: the profile or EDEN_NX2_GPU_WAITS.
inline bool g_report_enabled = g_enabled || g_waits;

/// Called by VideoCore::CreateGPU: static initialization runs before the frontend loads
/// nx2_switches.txt, so a switch set only there takes effect from that point on.
inline void ReadSwitch() {
    g_enabled = std::getenv("EDEN_NX2_GPU_PROFILE") != nullptr &&
                std::getenv("EDEN_NX2_SAMPLE_PROFILE") == nullptr;
    g_waits = ReadWaitsEnv();
    StutterTrace::ReadSwitch();
    g_zones_enabled = g_enabled || StutterTrace::Enabled();
    g_wait_zones_enabled = g_zones_enabled || g_waits;
    g_report_enabled = g_enabled || g_waits;
    Common::g_count_spin_contention = g_enabled;
}

/// The zones EDEN_NX2_GPU_WAITS times: the thread waits for something else (the guest, a
/// release, the other half of the split, the host GPU, the worker or the swapchain). IdleHook is
/// work, but it runs inside the back's split wait, which is only honest idle time without it.
constexpr bool IsWaitZone(Zone zone) {
    switch (zone) {
    case Zone::Idle:
    case Zone::PendingSleep:
    case Zone::SplitDrain:
    case Zone::SplitWait:
    case Zone::WriterWait:
    case Zone::ListSubmitWait:
    case Zone::ListGpuWait:
    case Zone::GpuWait:
    case Zone::PresentWait:
    case Zone::PipelineWait:
    case Zone::IdleHook:
        return true;
    default:
        return false;
    }
}

inline bool Enabled() {
    return g_enabled;
}

// Nanoseconds; from the host's own counter where the wall clock reads it directly (the zones
// run ~150,000 times a second, and QueryPerformanceCounter cost ~5% of the GPU thread).
inline u64 Now() {
    return static_cast<u64>(Common::g_wall_clock.GetTimeNS().count());
}

// What the zones and drain waits measure in: the time stamp counter without the fences the wall
// clock reads it with (those fences cost ~10% of the GPU back thread with ~45,000 draws a
// second). Report converts it with the wall clock.
inline u64 Ticks() {
#ifdef ARCHITECTURE_x86_64
    return __rdtsc();
#else
    return Now();
#endif
}

struct ThreadData {
    std::array<u64, static_cast<size_t>(Zone::Count)> exclusive_ns{};
    std::array<u64, static_cast<size_t>(Zone::Count)> count{};
    std::array<u64, 8> counters{}; ///< See Counter
    std::array<u64, static_cast<size_t>(DrainSite::Count)> drain_ns{};
    std::array<u64, static_cast<size_t>(DrainSite::Count)> drain_count{};
    /// GPU waits by the zone they happened in (Other: outside every zone)
    std::array<u64, static_cast<size_t>(Zone::Count)> gpu_wait_ns{};
    std::array<u64, static_cast<size_t>(Zone::Count)> gpu_wait_count{};
    Zone current{Zone::Other};
    u64 child_ns{};    ///< Time of the zones nested in the current one
    u64 window_start{};
    u64 window_start_ticks{};
    std::optional<u64> cpu_start_ns;
};

inline thread_local ThreadData t_data;

class Scope {
public:
    // `zone` is a constant at every site, so this folds to one load and branch of one flag.
    explicit Scope(Zone zone_)
        : zone{zone_}, enabled{IsWaitZone(zone_) ? g_wait_zones_enabled : g_zones_enabled} {
        if (!enabled) {
            return;
        }
        parent_child_ns = t_data.child_ns;
        t_data.child_ns = 0;
        parent = t_data.current;
        t_data.current = zone;
        start = Ticks();
        traced = StutterTrace::Enabled();
        if (traced) {
            outer = StutterTrace::Open(stutter_categories[static_cast<size_t>(zone)], start,
                                       static_cast<size_t>(zone));
        }
    }

    ~Scope() {
        if (!enabled) {
            return;
        }
        const u64 end = Ticks();
        const u64 elapsed = end - start;
        auto& data = t_data;
        const auto index = static_cast<size_t>(zone);
        const u64 exclusive = elapsed - data.child_ns;
        data.exclusive_ns[index] += exclusive;
        ++data.count[index];
        data.child_ns = parent_child_ns + elapsed;
        data.current = parent;
        if (zone == Zone::GpuWait) {
            data.gpu_wait_ns[static_cast<size_t>(parent)] += elapsed;
            ++data.gpu_wait_count[static_cast<size_t>(parent)];
        }
        if (traced) {
            // Time outside every zone is no zone's: it may be a wait no zone covers.
            StutterTrace::Close(outer, end,
                                parent == Zone::Other ? StutterTrace::ZoneSlots
                                                      : static_cast<size_t>(parent));
        }
    }

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Zone zone;
    Zone parent{Zone::Other};
    bool enabled;
    bool traced{};
    StutterTrace::Category outer{StutterTrace::Category::None};
    u64 start{};
    u64 parent_child_ns{};
};

/// Amounts counted on the GPU thread, logged per second with the zones.
enum class Counter : u32 {
    DownloadBytes,  ///< Buffer data copied back from the host GPU at fences
    DownloadCopies, ///< Buffer ranges copied back
    DmaReadBytes,   ///< Guest memory the native MME read with DMA
    DownloadBytesOnDmaPages, ///< Of DownloadBytes: on pages the MME has DMA-read
    MethodWrites,   ///< 3D register writes (Maxwell3D::CallMethod below the macro range)
    SameValueWrites, ///< Of MethodWrites: the register already held the value
    AcquireChecks,  ///< Semaphore acquires evaluated
    AcquireWaits,   ///< Of AcquireChecks: not met yet (the channel waits)
};

/// Method calls on this thread by method (3D registers on the back, macros on the front), and
/// same-value writes among them; MME DMA reads by the macro that made them. The most frequent
/// are logged with the zones.
inline thread_local std::array<u32, 0x2000> t_method_count{};
inline thread_local std::array<u32, 0x2000> t_method_same{};
inline thread_local std::array<u32, 0x2000> t_dma_reads{};

inline void CountMethod(u32 method, bool same) {
    if (Enabled() && method < t_method_count.size()) {
        ++t_method_count[method];
        t_method_same[method] += same ? 1 : 0;
    }
}

inline void CountDmaRead(u32 macro) {
    if (Enabled() && macro < t_dma_reads.size()) {
        ++t_dma_reads[macro];
    }
}

inline std::string TopMethods(const std::array<u32, 0x2000>& counts,
                              const std::array<u32, 0x2000>* same, u64 window) {
    std::array<u32, 0x2000> order;
    for (u32 i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    constexpr size_t top = 24;
    std::partial_sort(order.begin(), order.begin() + top, order.end(),
                      [&counts](u32 a, u32 b) { return counts[a] > counts[b]; });
    std::string text;
    for (size_t i = 0; i < top && counts[order[i]] != 0; ++i) {
        const u32 method = order[i];
        text += fmt::format(" {:#x} {:.0f}/s", method,
                            static_cast<double>(counts[method]) * 1e9 / window);
        if (same != nullptr && (*same)[method] != 0) {
            text += fmt::format(" ({:.0f} same)", static_cast<double>((*same)[method]) * 1e9 / window);
        }
        text += ',';
    }
    if (!text.empty()) {
        text.pop_back();
    }
    return text;
}

/// Device pages (4 KiB) the native MME has DMA-read since start (GPU thread only; filled only
/// while profiling): which buffer downloads anything on the GPU thread reads back.
inline std::unordered_set<u64>& DmaReadPages() {
    static std::unordered_set<u64> pages;
    return pages;
}

inline void Add(Counter counter, u64 amount) {
    if (Enabled()) {
        t_data.counters[static_cast<size_t>(counter)] += amount;
    }
}

/// Whether AddDrain counts: the caller takes `start` only then.
inline bool CountsDrains() {
    return g_report_enabled;
}

/// The front waited for the back since `start` (Ticks) for `site`.
inline void AddDrain(DrainSite site, u64 start) {
    if (CountsDrains()) {
        const auto index = static_cast<size_t>(site);
        t_data.drain_ns[index] += Ticks() - start;
        ++t_data.drain_count[index];
    }
}

/// Which EDEN_NX2_GPU_WAITS line a thread logs from Report (None: none).
enum class WaitsLine : u8 { None, Front, Back };

/// The EDEN_NX2_GPU_WAITS line of one 10 s window: `data`'s zone times are Ticks and its counts
/// calls, over `window_ns` of wall time that is `window_ticks` long; `cpu_ns` is the thread's CPU
/// time in it. Times are ms per second of wall time with one decimal ("12.3 ms"), counts calls
/// per second without decimals ("456/s"). Every field is always present, in this order; fields
/// are separated by " | ". host_cpu reads "host_cpu unavailable" where the OS can't tell.
///
/// Front (the GPU thread):
///   GPU waits per second: work W ms | wait back B ms (drain D ms N/s, writer R ms N/s) |
///   wait releases P ms (N/s) | wait guest I ms (N/s) | wait list copies L ms (N/s) |
///   wait GPU G ms (N/s) | wait other O ms (N/s) | host_cpu C ms | window T ms |
///   drains N/s: SITE n/s x ms, SITE n/s x ms, ...
/// (one line; "drains N/s: none" when there were none). SITE is a drain_site_names entry (lower
/// case, may contain spaces: "acquire", "list writers", "fence release", "map", "list flush", ...),
/// listed only when it waited; its times are those the front waited at that site and add up to
/// about wait back.
///   work = window - every wait; wait back = drain (Drain, a full ring: split drain) + writer
///   (WaitFor: writer wait); wait releases = pending sleep (suspended channels wait for a
///   semaphore release or list copy; includes their hardware polling); wait guest = idle (no
///   command queued); wait list copies = list submit wait + list GPU wait; wait GPU = GPU wait;
///   wait other = any other wait zone (none expected).
///
/// Back (the split's back thread):
///   GPU back waits per second: busy X ms | idle Y ms (N/s) | idle hook H ms (N/s) |
///   present wait P ms (N/s) | GPU wait G ms (N/s) | pipeline wait Q ms (N/s) |
///   wait other O ms (N/s) | host_cpu C ms | window T ms
///   busy = window - idle - present wait - GPU wait - pipeline wait - wait other, so it includes
///   the idle hook; idle = split wait without the idle hook (nothing to do); idle hook =
///   Scheduler::FlushDeferred and its submit, run when the ring empties or the front asks for it
///   (RequestIdleHook); GPU wait = GPU wait + list GPU wait + list submit wait; wait other = the
///   front's wait zones (none expected).
inline std::string FormatWaitsLine(const ThreadData& data, WaitsLine kind, u64 window_ns,
                                   u64 window_ticks, std::optional<u64> cpu_ns) {
    window_ns = (std::max<u64>)(window_ns, 1);
    window_ticks = (std::max<u64>)(window_ticks, 1);
    const auto ms = [window_ticks](u64 ticks) {
        return static_cast<double>(ticks) * 1000.0 / static_cast<double>(window_ticks);
    };
    const auto per_s = [window_ns](u64 count) {
        return static_cast<double>(count) * 1e9 / static_cast<double>(window_ns);
    };
    struct Sum {
        u64 ticks{};
        u64 count{};
    };
    const auto sum = [&data](std::initializer_list<Zone> zones) {
        Sum total;
        for (const Zone zone : zones) {
            total.ticks += data.exclusive_ns[static_cast<size_t>(zone)];
            total.count += data.count[static_cast<size_t>(zone)];
        }
        return total;
    };
    Sum waits;
    for (size_t i = 0; i < data.exclusive_ns.size(); ++i) {
        if (IsWaitZone(static_cast<Zone>(i)) && static_cast<Zone>(i) != Zone::IdleHook) {
            waits.ticks += data.exclusive_ns[i];
            waits.count += data.count[i];
        }
    }
    const auto field = [&ms, &per_s](const char* name, Sum value) {
        return fmt::format(" | {} {:.1f} ms ({:.0f}/s)", name, ms(value.ticks), per_s(value.count));
    };
    const u64 work = window_ticks > waits.ticks ? window_ticks - waits.ticks : 0;
    std::string line;
    if (kind == WaitsLine::Back) {
        const Sum idle = sum({Zone::SplitWait});
        const Sum present = sum({Zone::PresentWait});
        const Sum gpu = sum({Zone::GpuWait, Zone::ListGpuWait, Zone::ListSubmitWait});
        const Sum pipeline = sum({Zone::PipelineWait});
        const Sum other{
            waits.ticks - idle.ticks - present.ticks - gpu.ticks - pipeline.ticks,
            waits.count - idle.count - present.count - gpu.count - pipeline.count};
        line = fmt::format("GPU back waits per second: busy {:.1f} ms", ms(work));
        line += field("idle", idle);
        line += field("idle hook", sum({Zone::IdleHook}));
        line += field("present wait", present);
        line += field("GPU wait", gpu);
        line += field("pipeline wait", pipeline);
        line += field("wait other", other);
    } else {
        const Sum drain = sum({Zone::SplitDrain});
        const Sum writer = sum({Zone::WriterWait});
        const Sum releases = sum({Zone::PendingSleep});
        const Sum guest = sum({Zone::Idle});
        const Sum lists = sum({Zone::ListSubmitWait, Zone::ListGpuWait});
        const Sum gpu = sum({Zone::GpuWait});
        const Sum other{waits.ticks - drain.ticks - writer.ticks - releases.ticks - guest.ticks -
                            lists.ticks - gpu.ticks,
                        waits.count - drain.count - writer.count - releases.count - guest.count -
                            lists.count - gpu.count};
        line = fmt::format("GPU waits per second: work {:.1f} ms", ms(work));
        line += fmt::format(" | wait back {:.1f} ms (drain {:.1f} ms {:.0f}/s, writer {:.1f} ms "
                            "{:.0f}/s)",
                            ms(drain.ticks + writer.ticks), ms(drain.ticks), per_s(drain.count),
                            ms(writer.ticks), per_s(writer.count));
        line += field("wait releases", releases);
        line += field("wait guest", guest);
        line += field("wait list copies", lists);
        line += field("wait GPU", gpu);
        line += field("wait other", other);
    }
    if (cpu_ns) {
        line += fmt::format(" | host_cpu {:.1f} ms", static_cast<double>(*cpu_ns) * 1000.0 /
                                                         static_cast<double>(window_ns));
    } else {
        line += " | host_cpu unavailable";
    }
    line += fmt::format(" | window {:.1f} ms", static_cast<double>(window_ns) / 1e6);
    if (kind != WaitsLine::Back) {
        std::string sites;
        u64 drains{};
        for (size_t i = 0; i < data.drain_count.size(); ++i) {
            if (data.drain_count[i] == 0) {
                continue;
            }
            drains += data.drain_count[i];
            sites += fmt::format("{}{} {:.0f}/s {:.1f} ms", sites.empty() ? "" : ", ",
                                 drain_site_names[i], per_s(data.drain_count[i]),
                                 ms(data.drain_ns[i]));
        }
        line += fmt::format(" | drains {:.0f}/s: {}", per_s(drains),
                            sites.empty() ? "none" : sites);
    }
    return line;
}

/// Called by the GPU thread (and the split's back thread) between commands: logs and resets
/// the totals every 10 s.
/// `report_all_contention`: also log (and reset) every thread's spin mutex contention.
/// `waits_line`: the EDEN_NX2_GPU_WAITS line this thread logs.
inline void Report(const char* thread_name = "GPU thread", bool report_all_contention = false,
                   WaitsLine waits_line = WaitsLine::None) {
    if (!g_report_enabled) {
        return;
    }
    const bool waits = g_waits && waits_line != WaitsLine::None;
    if (!g_enabled && !waits) {
        return;
    }
    auto& data = t_data;
    const u64 now = Now();
    if (data.window_start == 0) {
        data.window_start = now;
        data.window_start_ticks = Ticks();
        data.cpu_start_ns = Common::CurrentThreadCpuTimeNs();
        data.child_ns = 0;
        return;
    }
    const u64 window = now - data.window_start;
    if (window < 10'000'000'000ULL) {
        return;
    }
    const auto cpu_now = Common::CurrentThreadCpuTimeNs();
    const u64 now_ticks = Ticks();
    const u64 window_ticks = (std::max<u64>)(now_ticks - data.window_start_ticks, 1);
    // Time in no zone at all is the wall time minus the zones' totals.
    u64 zoned{};
    for (size_t i = 1; i < data.exclusive_ns.size(); ++i) {
        zoned += data.exclusive_ns[i];
    }
    data.exclusive_ns[0] = window_ticks > zoned ? window_ticks - zoned : 0;
    if (waits) {
        std::optional<u64> cpu_ns;
        if (cpu_now && data.cpu_start_ns && *cpu_now >= *data.cpu_start_ns) {
            cpu_ns = *cpu_now - *data.cpu_start_ns;
        }
        LOG_INFO(HW_GPU, "{}", FormatWaitsLine(data, waits_line, window, window_ticks, cpu_ns));
    }
    if (g_enabled) {
        std::string line;
        for (size_t i = 0; i < data.exclusive_ns.size(); ++i) {
            if (data.exclusive_ns[i] == 0 && data.count[i] == 0) {
                continue;
            }
            // Per second of wall time: milliseconds spent and calls made.
            const double ms = static_cast<double>(data.exclusive_ns[i]) * 1000.0 / window_ticks;
            const double calls = static_cast<double>(data.count[i]) * 1e9 / window;
            line += fmt::format(" | {} {:.1f} ms ({:.0f}/s)", zone_names[i], ms, calls);
        }
        line += fmt::format(" | buffer downloads {:.0f} KiB in {:.0f} copies ({:.0f} KiB on pages "
                            "MME DMA reads) | MME DMA reads {:.0f} KiB",
                            static_cast<double>(data.counters[0]) * 1e9 / 1024.0 / window,
                            static_cast<double>(data.counters[1]) * 1e9 / window,
                            static_cast<double>(data.counters[3]) * 1e9 / 1024.0 / window,
                            static_cast<double>(data.counters[2]) * 1e9 / 1024.0 / window);
        line += fmt::format(" | 3D register writes {:.0f} ({:.0f} same value)",
                            static_cast<double>(data.counters[4]) * 1e9 / window,
                            static_cast<double>(data.counters[5]) * 1e9 / window);
        line += fmt::format(" | acquires {:.0f} ({:.0f} waited)",
                            static_cast<double>(data.counters[6]) * 1e9 / window,
                            static_cast<double>(data.counters[7]) * 1e9 / window);
        std::string drains;
        for (size_t i = 0; i < data.drain_count.size(); ++i) {
            if (data.drain_count[i] != 0) {
                drains += fmt::format(" {} {:.1f} ms ({:.0f}/s),", drain_site_names[i],
                                      static_cast<double>(data.drain_ns[i]) * 1000.0 / window_ticks,
                                      static_cast<double>(data.drain_count[i]) * 1e9 / window);
            }
        }
        if (!drains.empty()) {
            drains.pop_back();
            line += " | waits for the back by site:" + drains;
        }
        std::string gpu_waits;
        for (size_t i = 0; i < data.gpu_wait_count.size(); ++i) {
            if (data.gpu_wait_count[i] != 0) {
                gpu_waits += fmt::format(" {} {:.1f} ms ({:.0f}/s),", zone_names[i],
                                         static_cast<double>(data.gpu_wait_ns[i]) * 1000.0 / window_ticks,
                                         static_cast<double>(data.gpu_wait_count[i]) * 1e9 / window);
            }
        }
        if (!gpu_waits.empty()) {
            gpu_waits.pop_back();
            line += " | GPU waits by zone:" + gpu_waits;
        }
        {
            auto& own = Common::t_spin_contention;
            auto& all = Common::g_spin_contention;
            line += fmt::format(
                " | spin mutex contention: this thread {:.0f}/s {:.1f} ms ({:.0f} sleeps/s)",
                static_cast<double>(own.count) * 1e9 / window, static_cast<double>(own.ns) / 1e6 * 1e9 / window,
                static_cast<double>(own.sleeps) * 1e9 / window);
            own = {};
            if (report_all_contention) {
                const u64 count = all.count.exchange(0, std::memory_order_relaxed);
                const u64 ns = all.ns.exchange(0, std::memory_order_relaxed);
                const u64 sleeps = all.sleeps.exchange(0, std::memory_order_relaxed);
                line += fmt::format(", all threads {:.0f}/s {:.1f} ms ({:.0f} sleeps/s)",
                                    static_cast<double>(count) * 1e9 / window,
                                    static_cast<double>(ns) / 1e6 * 1e9 / window,
                                    static_cast<double>(sleeps) * 1e9 / window);
            }
        }
        if (cpu_now && data.cpu_start_ns && *cpu_now >= *data.cpu_start_ns) {
            line += fmt::format(" | host_cpu_busy_ms_per_s={:.3f} window_wall_ms={:.3f}",
                                static_cast<double>(*cpu_now - *data.cpu_start_ns) * 1000.0 / window,
                                static_cast<double>(window) / 1e6);
        } else {
            line += fmt::format(" | host_cpu_busy_ms_per_s=unavailable window_wall_ms={:.3f}",
                                static_cast<double>(window) / 1e6);
        }
        LOG_INFO(HW_GPU, "{} per second:{}", thread_name, line);
        if (const std::string methods = TopMethods(t_method_count, &t_method_same, window); !methods.empty()) {
            LOG_INFO(HW_GPU, "{} methods per second:{}", thread_name, methods);
        }
        if (const std::string reads = TopMethods(t_dma_reads, nullptr, window); !reads.empty()) {
            LOG_INFO(HW_GPU, "{} MME DMA reads per second by macro:{}", thread_name, reads);
        }
        t_method_count = {};
        t_method_same = {};
        t_dma_reads = {};
    }
    const Zone current = data.current;
    data = {};
    data.current = current;
    data.window_start = now;
    data.window_start_ticks = now_ticks;
    data.cpu_start_ns = cpu_now;
}

} // namespace VideoCommon::Nx2Profile
